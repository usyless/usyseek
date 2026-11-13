"""usyseek vision backend, run as a persistent worker process over Win32 shared memory + named pipe.

Interface (a custom script set in settings must provide the same functions):
    load(cfg: dict) -> dict
        cfg:  model_dir, model_name, device ("auto" | "cpu")
        returns: size (int), resize_mode ("squash" | "shortest" | "longest"), dim, device, backend
    embed(batch: numpy.ndarray[uint8, N x H x W x 3]) -> numpy.ndarray[float32, N x dim]   (L2 normalised)
    suggest_batch(limit: int) -> int
    benchmark_batch(limit: int) -> int   (fastest batch size that fits in VRAM, optional: falls back to suggest_batch)
    batch_hint() -> int        (0 = no limit; lowered automatically after an out-of-memory error)
    export_text(model_dir, model_name) -> str   (path of the text tower onnx, exported if missing)

The C++ side decodes and resizes images to size x size RGB (using resize_mode) directly into a Win32
pagefile-backed shared memory mapping, so this module reads uint8 pixels zero-copy from RAM,
normalises on the device, runs the vision tower, and writes float32 embeddings back into shared memory.
"""

import os
from contextlib import nullcontext
from pathlib import Path

_torch = None
_model = None
_device = "cpu"
_autocast = nullcontext()
_mean = None
_std = None
_dim = 0
_size = 0
_hint = 0

_BENCH_FIRST_SIZE = 8
_BENCH_WARMUP_MIN_RUNS = 2
_BENCH_WARMUP_MAX_RUNS = 8
_BENCH_WARMUP_TOLERANCE = 0.10
_BENCH_TIMED_MIN_SECONDS = 0.3
_BENCH_TIMED_MIN_RUNS = 3
_BENCH_TIMED_MAX_RUNS = 40
_BENCH_DROP_RATIO = 0.75
_BENCH_GAIN_RATIO = 1.03


def _model_paths(model_dir, model_name):
    d = Path(model_dir)
    return d / f"{model_name}.safetensors", d / f"{model_name}-text.onnx"


def _preprocess_cfg(open_clip, model_name, model):
    """PE models use squash/bilinear/0.5 normalisation; read the pretrained cfg instead of open_clip's defaults."""
    cfg = dict(getattr(model.visual, "preprocess_cfg", {}) or {})
    try:
        for tag in open_clip.list_pretrained_tags_by_model(model_name):
            pcfg = open_clip.get_pretrained_cfg(model_name, tag)
            for k in ("mean", "std", "interpolation", "resize_mode"):
                if pcfg.get(k) is not None:
                    cfg[k] = pcfg[k]
            break
    except Exception:
        pass
    return cfg


def load(cfg):
    global _torch, _model, _device, _autocast, _mean, _std, _dim, _size
    import open_clip
    import torch
    from safetensors import safe_open

    _torch = torch
    model_name = cfg["model_name"]
    weights, _ = _model_paths(cfg["model_dir"], model_name)
    if not weights.exists():
        raise FileNotFoundError(f"missing weights: {weights}")

    want_gpu = cfg.get("device", "auto") != "cpu"
    if want_gpu and torch.cuda.is_available():
        _device = "cuda"
        _autocast = torch.autocast("cuda")
    else:
        _device = "cpu"
        _autocast = nullcontext()
        torch.set_num_threads(max(1, (os.cpu_count() or 4) - 2))

    with torch.device("meta"):
        model = open_clip.create_model(model_name, pretrained=None, device="meta")

    # Drop the text tower so it never touches VRAM
    if hasattr(model, "text"):
        model.text = None
    else:
        for attr in ("transformer", "token_embedding", "ln_final", "text_projection", "positional_embedding"):
            if hasattr(model, attr):
                setattr(model, attr, None)

    state = {}
    with safe_open(str(weights), framework="pt", device=_device) as f:
        for k in f.keys():
            if k.startswith("visual."):
                t = f.get_tensor(k)
                if _device == "cuda" and t.is_floating_point():
                    t = t.half()
                state[k] = t
    model.load_state_dict(state, assign=True, strict=False)
    for name, buf in list(model.named_buffers()):
        if buf.is_meta and name.startswith("visual."):
            parent_name, _, attr = name.rpartition(".")
            parent = model.get_submodule(parent_name) if parent_name else model
            setattr(parent, attr, torch.zeros(buf.shape, dtype=buf.dtype, device=_device))
    for mod in model.visual.modules():
        if hasattr(mod, "init_non_persistent_buffers"):
            mod.init_non_persistent_buffers()
    model.eval()
    _model = model

    pcfg = _preprocess_cfg(open_clip, model_name, model)
    size = pcfg.get("size", 224)
    _size = int(size[0] if isinstance(size, (tuple, list)) else size)
    dtype = torch.float16 if _device == "cuda" else torch.float32
    _mean = torch.tensor(pcfg.get("mean", (0.5, 0.5, 0.5)), device=_device, dtype=dtype).view(1, 3, 1, 1) * 255.0
    _std = torch.tensor(pcfg.get("std", (0.5, 0.5, 0.5)), device=_device, dtype=dtype).view(1, 3, 1, 1) * 255.0

    with torch.inference_mode(), _autocast:
        probe = torch.zeros((1, 3, _size, _size), device=_device, dtype=dtype)
        _dim = int(model.encode_image(probe).shape[-1])

    if _device == "cuda":
        name = torch.cuda.get_device_name(0)
        backend = "ROCm" if getattr(torch.version, "hip", None) else "CUDA"
    else:
        name = "CPU"
        backend = "CPU"

    return {
        "size": _size,
        "resize_mode": pcfg.get("resize_mode", "shortest"),
        "dim": _dim,
        "device": name,
        "backend": backend,
        "fallback_cpu": bool(want_gpu and _device == "cpu"),
        "torch": torch.__version__,
    }


def _run(batch):
    torch = _torch
    x = torch.from_numpy(batch).to(_device, non_blocking=False)
    x = x.permute(0, 3, 1, 2).to(_mean.dtype)
    x = (x - _mean) / _std
    with torch.inference_mode(), _autocast:
        feats = _model.encode_image(x).float()
        feats = torch.nn.functional.normalize(feats, dim=-1)
    return feats.cpu().numpy()


def embed(batch):
    """Splits the batch on out-of-memory and remembers the size that worked."""
    global _hint
    import numpy as np

    try:
        return _run(batch)
    except Exception as e:  # torch.OutOfMemoryError, HIP/CUDA OOM RuntimeError
        if _device != "cuda" or "out of memory" not in str(e).lower() or len(batch) <= 1:
            raise
        _torch.cuda.empty_cache()
        half = len(batch) // 2
        _hint = half if _hint == 0 else min(_hint, half)
        return np.concatenate([embed(batch[:half]), embed(batch[half:])], axis=0)


def batch_hint():
    return _hint


def suggest_batch(limit=128):
    """Measures VRAM per image with a small probe and fits the batch conservatively
    so Windows WDDM never spills into shared system RAM when other apps are open."""
    torch = _torch
    if _device != "cuda":
        return max(1, min(limit, 16))
    import numpy as np

    torch.cuda.empty_cache()
    base_alloc = torch.cuda.memory_allocated()
    base_res = torch.cuda.memory_reserved()
    probe_n = 8
    torch.cuda.reset_peak_memory_stats()
    _run(np.zeros((probe_n, _size, _size, 3), dtype=np.uint8))
    peak_delta = max(
        torch.cuda.max_memory_allocated() - base_alloc,
        torch.cuda.max_memory_reserved() - base_res,
    )
    # 2x safety factor for MIOpen/cuBLAS workspaces + caching allocator fragmentation,
    # with a minimum floor proportional to image resolution (~22 MB at 336x336).
    min_per_img = max(16 << 20, _size * _size * 3 * 64)
    per_image = max(min_per_img, int((peak_delta / probe_n) * 2.0))

    free, total = torch.cuda.mem_get_info()
    torch.cuda.empty_cache()
    model_bytes = max(torch.cuda.memory_allocated(), torch.cuda.memory_reserved())
    # On some Windows ROCm/HIP drivers mem_get_info() does not subtract this process's pool
    if free > total - model_bytes:
        free = max(0, total - model_bytes)

    # Leave at least 1.5 GiB or 25% of total VRAM untouched for Windows DWM & other open apps
    headroom = max(int(1.5 * (1 << 30)), int(total * 0.25))
    usable = max(0, free - headroom)
    raw = int((usable * 0.55) // per_image)
    # Snap to a clean multiple of 8 within [8, min(limit, 128)]
    capped = max(8, min(int(limit), 128, raw))
    return max(8, (capped // 8) * 8)


def candidate_sizes(cap):
    """Powers of two from the first benchmark size up to the cap, always ending on the cap."""
    sizes = []
    n = _BENCH_FIRST_SIZE
    while n < cap:
        sizes.append(n)
        n *= 2
    sizes.append(max(1, cap))
    return sizes


def warm_up(run_once):
    """Calls run_once (returning elapsed seconds) until two consecutive runs agree within
    _BENCH_WARMUP_TOLERANCE, after at least _BENCH_WARMUP_MIN_RUNS and at most _BENCH_WARMUP_MAX_RUNS runs."""
    previous = None
    for runs in range(1, _BENCH_WARMUP_MAX_RUNS + 1):
        elapsed = run_once()
        settled = previous is not None and abs(elapsed - previous) <= _BENCH_WARMUP_TOLERANCE * max(elapsed, previous)
        if settled and runs >= _BENCH_WARMUP_MIN_RUNS:
            return
        previous = elapsed


def timed_rate(run_once, size):
    """Calls run_once (returning elapsed seconds) until _BENCH_TIMED_MIN_SECONDS have been spent and at least
    _BENCH_TIMED_MIN_RUNS have run, never more than _BENCH_TIMED_MAX_RUNS. Returns images per second at the
    median run time, which a single stalled or unusually fast run cannot move."""
    times = []
    while len(times) < _BENCH_TIMED_MAX_RUNS and (len(times) < _BENCH_TIMED_MIN_RUNS or sum(times) < _BENCH_TIMED_MIN_SECONDS):
        times.append(run_once())
    times.sort()
    middle = len(times) // 2
    median = times[middle] if len(times) % 2 else (times[middle - 1] + times[middle]) / 2
    return size / median


def search_batch(sizes, measure):
    """Walks sizes in ascending order and returns the fastest. measure(size) gives images per second,
    or None when the size ran out of memory. Stops at the first out-of-memory size or once throughput
    falls below _BENCH_DROP_RATIO of the best. A larger size only wins by beating the best by _BENCH_GAIN_RATIO."""
    best_size = sizes[0]
    best_rate = 0.0
    for size in sizes:
        rate = measure(size)
        if rate is None:
            break
        if rate > best_rate * _BENCH_GAIN_RATIO:
            best_size = size
            best_rate = rate
        elif rate < best_rate * _BENCH_DROP_RATIO:
            break
    return best_size


def benchmark_batch(limit=128):
    """Times embedding at increasing batch sizes up to what suggest_batch fits in VRAM and returns the fastest.
    On CPU throughput barely depends on batch size, so the suggested size is kept."""
    cap = suggest_batch(limit)
    if _device != "cuda":
        return cap
    import time
    import numpy as np

    def measure(size):
        frames = np.zeros((size, _size, _size, 3), dtype=np.uint8)

        def run_once():
            start = time.perf_counter()
            _run(frames)
            return time.perf_counter() - start

        try:
            warm_up(run_once)
            return timed_rate(run_once, size)
        except Exception as e:  # torch.OutOfMemoryError, HIP/CUDA OOM RuntimeError
            if "out of memory" not in str(e).lower():
                raise
            return None

    try:
        return search_batch(candidate_sizes(cap), measure)
    finally:
        _torch.cuda.empty_cache()


def export_text(model_dir, model_name):
    """One-time export of the text tower to ONNX so the search UI never needs torch."""
    weights, onnx_path = _model_paths(model_dir, model_name)
    if onnx_path.exists():
        return str(onnx_path)

    import open_clip
    import torch
    from safetensors import safe_open

    with torch.device("meta"):
        m = open_clip.create_model(model_name, pretrained=None, device="meta")
    m.visual = None

    sd = {}
    with safe_open(str(weights), framework="pt", device="cpu") as f:
        for k in f.keys():
            if not k.startswith("visual."):
                sd[k] = f.get_tensor(k)
    m.load_state_dict(sd, assign=True, strict=False)
    for name, buf in list(m.named_buffers()):
        if buf.is_meta:
            parent_name, _, attr = name.rpartition(".")
            parent = m.get_submodule(parent_name) if parent_name else m
            if attr == "attn_mask" and hasattr(parent, "build_causal_mask"):
                setattr(parent, attr, parent.build_causal_mask().to("cpu"))
            else:
                setattr(parent, attr, torch.zeros(buf.shape, dtype=buf.dtype, device="cpu"))
    for mod in m.modules():
        if hasattr(mod, "init_non_persistent_buffers"):
            mod.init_non_persistent_buffers()
    m.eval()

    class TextWrapper(torch.nn.Module):
        def __init__(self, inner):
            super().__init__()
            self.inner = inner

        def forward(self, x):
            return self.inner.encode_text(x)

    ctx = getattr(m, "context_length", 32)
    tmp_path = onnx_path.with_suffix(".onnx.tmp")
    try:
        with torch.no_grad():
            torch.onnx.export(
                TextWrapper(m).eval(),
                (torch.zeros((1, ctx), dtype=torch.long),),
                str(tmp_path),
                input_names=["input_ids"],
                output_names=["text_embeddings"],
                opset_version=18,
            )
        os.replace(tmp_path, onnx_path)
    finally:
        if tmp_path.exists():
            try:
                tmp_path.unlink()
            except OSError:
                pass
    return str(onnx_path)


def _run_worker(pipe_name, custom_script=""):
    import importlib.util
    import json
    import mmap
    import sys
    import numpy as np

    target = sys.modules[__name__]
    if custom_script:
        spec = importlib.util.spec_from_file_location("usyseek_custom_vision", custom_script)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        target = mod

    shm_name = ""
    shm_bytes = 0
    shm_map = None
    size = 0
    dim = 0

    with open(pipe_name, "r+b", buffering=0) as pipe:
        buf = b""

        def read_msg():
            nonlocal buf
            while b"\n" not in buf:
                chunk = pipe.read(4096)
                if not chunk:
                    return None
                buf += chunk
            line, _, buf = buf.partition(b"\n")
            return json.loads(line.decode("utf-8"))

        def send_msg(obj):
            pipe.write((json.dumps(obj) + "\n").encode("utf-8"))

        while True:
            msg = read_msg()
            if msg is None:
                break
            cmd = msg.get("cmd")
            if cmd == "quit":
                break
            try:
                if cmd == "load":
                    info = target.load(msg["cfg"])
                    size = int(info["size"])
                    dim = int(info["dim"])
                    send_msg({"ok": True, "info": info})
                elif cmd == "export_text":
                    p = target.export_text(msg["model_dir"], msg["model_name"]) if hasattr(target, "export_text") else ""
                    send_msg({"ok": True, "path": str(p)})
                elif cmd == "suggest_batch":
                    b = int(target.suggest_batch(int(msg.get("limit", 128)))) if hasattr(target, "suggest_batch") else 16
                    send_msg({"ok": True, "batch": b})
                elif cmd == "benchmark_batch":
                    fn = target.benchmark_batch if hasattr(target, "benchmark_batch") else target.suggest_batch
                    b = int(fn(int(msg.get("limit", 128)))) if hasattr(target, "suggest_batch") else 16
                    send_msg({"ok": True, "batch": b})
                elif cmd == "embed":
                    req_shm = msg["shm"]
                    req_bytes = int(msg["shm_bytes"])
                    frames = int(msg["frames"])
                    if req_shm != shm_name or req_bytes != shm_bytes or shm_map is None:
                        if shm_map is not None:
                            try:
                                shm_map.close()
                            except BufferError:
                                pass
                        shm_map = mmap.mmap(-1, req_bytes, tagname=req_shm, access=mmap.ACCESS_WRITE)
                        shm_name = req_shm
                        shm_bytes = req_bytes
                    in_count = frames * size * size * 3
                    out_count = frames * dim
                    arr = np.frombuffer(shm_map, dtype=np.uint8, count=in_count).reshape((frames, size, size, 3))
                    try:
                        emb = np.ascontiguousarray(target.embed(arr), dtype=np.float32)
                    finally:
                        del arr
                    out_arr = np.frombuffer(shm_map, dtype=np.float32, count=out_count)
                    try:
                        out_arr[:] = emb.ravel()[:out_count]
                    finally:
                        del out_arr
                        del emb
                    hint = int(target.batch_hint()) if hasattr(target, "batch_hint") else 0
                    send_msg({"ok": True, "hint": hint})
                else:
                    send_msg({"ok": False, "error": f"unknown cmd: {cmd}"})
            except Exception as e:
                send_msg({"ok": False, "error": f"{type(e).__name__}: {e}"})

    if shm_map is not None:
        try:
            shm_map.close()
        except BufferError:
            pass


if __name__ == "__main__":
    import sys

    if len(sys.argv) >= 3 and sys.argv[1] == "--worker":
        _run_worker(sys.argv[2], sys.argv[3] if len(sys.argv) >= 4 else "")
    elif len(sys.argv) >= 4 and sys.argv[1] == "--export-text":
        export_text(sys.argv[2], sys.argv[3])
