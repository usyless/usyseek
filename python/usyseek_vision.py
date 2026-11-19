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
from contextlib import nullcontext, suppress
from pathlib import Path

_torch = None
_np = None
_model = None
_device = "cpu"
_autocast = nullcontext()
_mean = None
_inv_std = None
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


def _is_oom(exc):
    oom_cls = getattr(_torch, "OutOfMemoryError", None)
    return (oom_cls is not None and isinstance(exc, oom_cls)) or "out of memory" in str(exc).lower()


def _channel_triplet(value, default):
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return (float(value), float(value), float(value))
    vals = tuple(float(v) for v in value)
    if len(vals) == 1:
        return vals * 3
    if len(vals) != 3:
        raise ValueError(f"expected 3 channel values, got {len(vals)}")
    return vals


def load(cfg):
    global _torch, _np, _model, _device, _autocast, _mean, _inv_std, _dim, _size, _hint
    import numpy as np
    import open_clip
    import torch
    from safetensors import safe_open

    _torch = torch
    _np = np
    _hint = 0
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
        for k in f.keys():  # noqa: SIM118
            if k.startswith("visual."):
                t = f.get_tensor(k)
                if _device == "cuda" and t.is_floating_point():
                    t = t.half()
                state[k] = t
    if not state:
        raise RuntimeError(f"no visual.* weights found in {weights}")
    model.load_state_dict(state, assign=True, strict=False)
    meta_params = [name for name, p in model.named_parameters() if p.is_meta and name.startswith("visual.")]
    if meta_params:
        raise RuntimeError(f"missing visual weights for {model_name} (e.g. {meta_params[0]})")
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
    size = pcfg.get("size") or getattr(model.visual, "image_size", None) or 224
    _size = int(size[0] if isinstance(size, (tuple, list)) else size)
    dtype = torch.float16 if _device == "cuda" else torch.float32
    mean = _channel_triplet(pcfg.get("mean"), (0.5, 0.5, 0.5))
    std = _channel_triplet(pcfg.get("std"), (0.5, 0.5, 0.5))
    if any(s == 0.0 for s in std):
        raise ValueError(f"invalid zero std in preprocess_cfg: {std}")
    _mean = (torch.tensor(mean, device=_device, dtype=torch.float32).view(1, 3, 1, 1) * 255.0).to(dtype)
    _inv_std = (1.0 / (torch.tensor(std, device=_device, dtype=torch.float32).view(1, 3, 1, 1) * 255.0)).to(dtype)

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
        "resize_mode": pcfg.get("resize_mode") or "shortest",
        "dim": _dim,
        "device": name,
        "backend": backend,
        "fallback_cpu": bool(want_gpu and _device == "cpu"),
        "torch": torch.__version__,
    }


def _run(batch):
    torch = _torch
    with torch.inference_mode():
        x = torch.from_numpy(batch).to(_device, non_blocking=False)
        x = x.permute(0, 3, 1, 2).to(_mean.dtype)
        x.sub_(_mean).mul_(_inv_std)
        with _autocast:
            feats = _model.encode_image(x).float()
            feats = torch.nn.functional.normalize(feats, dim=-1)
    return feats.cpu().numpy()


def embed(batch):
    """Splits the batch on out-of-memory and remembers the size that worked."""
    global _hint, _np
    np = _np
    if np is None:
        import numpy as np

        _np = np

    if len(batch) == 0:
        return np.empty((0, _dim), dtype=np.float32)

    try:
        return _run(batch)
    except Exception as e:  # torch.OutOfMemoryError, HIP/CUDA OOM RuntimeError
        if _device != "cuda" or not _is_oom(e) or len(batch) <= 1:
            raise

    # Outside `except` so `e.__traceback__` no longer pins GPU tensors from the failed run.
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
    limit = max(1, int(limit))
    if _device != "cuda":
        return min(limit, 16)
    import numpy as np

    probe_n = min(8, limit)
    probe_oom = False
    while True:
        torch.cuda.empty_cache()
        base_alloc = torch.cuda.memory_allocated()
        base_res = torch.cuda.memory_reserved()
        torch.cuda.reset_peak_memory_stats()
        try:
            _run(np.zeros((probe_n, _size, _size, 3), dtype=np.uint8))
            break
        except Exception as e:  # torch.OutOfMemoryError, HIP/CUDA OOM RuntimeError
            if not _is_oom(e) or probe_n <= 1:
                raise
        # Outside `except` so failed probe tensors are freed before retrying smaller.
        probe_oom = True
        probe_n = max(1, probe_n // 2)

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
    upper = min(limit, 128, probe_n) if probe_oom else min(limit, 128)
    if upper < 8:
        return max(1, min(upper, raw))
    # Snap to a clean multiple of 8 within [8, upper]
    capped = max(8, min(upper, raw))
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
    return size / max(median, 1e-9)


def search_batch(sizes, measure):
    """Walks sizes in ascending order and returns the fastest. measure(size) gives images per second,
    or None when the size ran out of memory. Stops at the first out-of-memory size or once throughput
    falls below _BENCH_DROP_RATIO of the best. A larger size only wins by beating the best by _BENCH_GAIN_RATIO."""
    if not sizes:
        raise ValueError("sizes must not be empty")
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
            if not _is_oom(e):
                raise
            return None

    try:
        return search_batch(candidate_sizes(cap), measure)
    finally:
        _torch.cuda.empty_cache()


def _build_text_meta_buffer(torch, parent, attr, buf):
    if attr == "attn_mask":
        if hasattr(parent, "build_causal_mask"):
            return parent.build_causal_mask().to(device="cpu", dtype=buf.dtype)
        if buf.ndim == 2 and buf.shape[0] == buf.shape[1] and buf.is_floating_point():
            return torch.full(buf.shape, float("-inf"), dtype=buf.dtype, device="cpu").triu_(1)
    return torch.zeros(buf.shape, dtype=buf.dtype, device="cpu")


def export_text(model_dir, model_name):
    """One-time export of the text tower to ONNX so the search UI never needs torch."""
    weights, onnx_path = _model_paths(model_dir, model_name)
    if onnx_path.exists():
        return str(onnx_path)
    if not weights.exists():
        raise FileNotFoundError(f"missing weights: {weights}")

    import tempfile
    import open_clip
    import torch
    from safetensors import safe_open

    with torch.device("meta"):
        m = open_clip.create_model(model_name, pretrained=None, device="meta")
    m.visual = None

    sd = {}
    with safe_open(str(weights), framework="pt", device="cpu") as f:
        for k in f.keys():  # noqa: SIM118
            if not k.startswith("visual."):
                sd[k] = f.get_tensor(k)
    if not sd:
        raise RuntimeError(f"no text weights found in {weights}")
    m.load_state_dict(sd, assign=True, strict=False)
    for name, buf in list(m.named_buffers()):
        if buf.is_meta:
            parent_name, _, attr = name.rpartition(".")
            parent = m.get_submodule(parent_name) if parent_name else m
            setattr(parent, attr, _build_text_meta_buffer(torch, parent, attr, buf))
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

    ctx = getattr(m, "context_length", None) or getattr(getattr(m, "text", None), "context_length", None) or 32
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    # Export into a sibling temp dir with the final filename so any external `.onnx.data`
    # sidecar is referenced by its final name (`<model>-text.onnx.data`) and published atomically.
    with tempfile.TemporaryDirectory(dir=onnx_path.parent, prefix=f".{onnx_path.name}.") as tmp_dir:
        tmp_onnx = Path(tmp_dir) / onnx_path.name
        with torch.no_grad():
            torch.onnx.export(
                TextWrapper(m).eval(),
                (torch.zeros((1, ctx), dtype=torch.long),),
                str(tmp_onnx),
                input_names=["input_ids"],
                output_names=["text_embeddings"],
                opset_version=18,
            )
        tmp_data = tmp_onnx.with_name(f"{onnx_path.name}.data")
        final_data = onnx_path.with_name(f"{onnx_path.name}.data")
        if tmp_data.exists():
            os.replace(tmp_data, final_data)
        elif final_data.exists():
            with suppress(OSError):
                final_data.unlink()
        os.replace(tmp_onnx, onnx_path)
    return str(onnx_path)


def _load_target(custom_script):
    import importlib.util
    import sys

    if not custom_script:
        return sys.modules[__name__]
    spec = importlib.util.spec_from_file_location("usyseek_custom_vision", custom_script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load custom vision script: {custom_script}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run_worker(pipe_name, custom_script=""):
    import json
    import mmap
    import numpy as np

    target = None
    target_err = None
    shm_name = ""
    shm_bytes = 0
    shm_map = None
    size = 0
    dim = 0

    try:
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
                data = (json.dumps(obj, allow_nan=False) + "\n").encode("utf-8")
                view = memoryview(data)
                while view:
                    n = pipe.write(view)
                    if not n:
                        raise BrokenPipeError("pipe write returned 0 bytes")
                    view = view[n:]

            try:
                target = _load_target(custom_script)
            except Exception as e:
                target_err = f"{type(e).__name__}: {e}"

            while True:
                msg = read_msg()
                if msg is None:
                    break
                cmd = msg.get("cmd")
                if cmd == "quit":
                    break
                if target_err is not None:
                    send_msg({"ok": False, "error": target_err})
                    continue
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
                        b = max(1, int(target.suggest_batch(int(msg.get("limit", 128))))) if hasattr(target, "suggest_batch") else 16
                        send_msg({"ok": True, "batch": b})
                    elif cmd == "benchmark_batch":
                        fn = getattr(target, "benchmark_batch", None) or getattr(target, "suggest_batch", None)
                        b = max(1, int(fn(int(msg.get("limit", 128))))) if fn is not None else 16
                        send_msg({"ok": True, "batch": b})
                    elif cmd == "embed":
                        if size <= 0 or dim <= 0:
                            raise RuntimeError("embed called before load")
                        req_shm = msg["shm"]
                        req_bytes = int(msg["shm_bytes"])
                        frames = int(msg["frames"])
                        if frames <= 0:
                            raise ValueError(f"invalid frame count: {frames}")
                        in_count = frames * size * size * 3
                        out_count = frames * dim
                        if in_count > req_bytes or out_count * 4 > req_bytes:
                            raise ValueError(f"shared memory too small ({req_bytes} bytes) for {frames} frames")
                        if req_shm != shm_name or req_bytes != shm_bytes or shm_map is None:
                            if shm_map is not None:
                                with suppress(BufferError):
                                    shm_map.close()
                                shm_map = None
                                shm_name = ""
                                shm_bytes = 0
                            shm_map = mmap.mmap(-1, req_bytes, tagname=req_shm, access=mmap.ACCESS_WRITE)
                            shm_name = req_shm
                            shm_bytes = req_bytes
                        arr = np.frombuffer(shm_map, dtype=np.uint8, count=in_count).reshape((frames, size, size, 3))
                        try:
                            emb = np.ascontiguousarray(target.embed(arr), dtype=np.float32)
                        finally:
                            del arr
                        if emb.size != out_count:
                            raise ValueError(f"embed returned {emb.size} floats, expected {out_count} ({frames}x{dim})")
                        out_arr = np.frombuffer(shm_map, dtype=np.float32, count=out_count)
                        try:
                            out_arr[:] = emb.ravel()
                        finally:
                            del out_arr
                            del emb
                        hint = max(0, int(target.batch_hint())) if hasattr(target, "batch_hint") else 0
                        send_msg({"ok": True, "hint": hint})
                    else:
                        send_msg({"ok": False, "error": f"unknown cmd: {cmd}"})
                except Exception as e:
                    send_msg({"ok": False, "error": f"{type(e).__name__}: {e}"})
    finally:
        if shm_map is not None:
            with suppress(BufferError):
                shm_map.close()


if __name__ == "__main__":
    import sys

    if os.name == "nt":
        import subprocess

        # Python runs headless inside CREATE_NO_WINDOW; ensure child processes spawned by
        # dependencies (e.g. torch/open_clip probes) do not flash a visible console window.
        _orig_popen_init = subprocess.Popen.__init__
        _no_window_mask = getattr(subprocess, "CREATE_NEW_CONSOLE", 0) | getattr(subprocess, "DETACHED_PROCESS", 0)
        _create_no_window = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)

        def _hidden_popen_init(self, *args, **kwargs):
            if len(args) > 13:
                flags = args[13]
                if not (flags & _no_window_mask):
                    args = (*args[:13], flags | _create_no_window, *args[14:])
            else:
                flags = kwargs.get("creationflags", 0)
                if not (flags & _no_window_mask):
                    kwargs["creationflags"] = flags | _create_no_window
            return _orig_popen_init(self, *args, **kwargs)

        subprocess.Popen.__init__ = _hidden_popen_init

    if len(sys.argv) >= 3 and sys.argv[1] == "--worker":
        _run_worker(sys.argv[2], sys.argv[3] if len(sys.argv) >= 4 else "")
    elif len(sys.argv) >= 4 and sys.argv[1] == "--export-text":
        export_text(sys.argv[2], sys.argv[3])
