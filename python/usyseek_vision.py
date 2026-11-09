"""usyseek vision backend, embedded in usyseek-indexer.exe through pybind11.

Interface (a custom script set in settings must provide the same functions):
    load(cfg: dict) -> dict
        cfg:  model_dir, model_name, device ("auto" | "cpu")
        returns: size (int), resize_mode ("squash" | "shortest" | "longest"), dim, device, backend
    embed(batch: numpy.ndarray[uint8, N x H x W x 3]) -> numpy.ndarray[float32, N x dim]   (L2 normalised)
    suggest_batch(limit: int) -> int
    batch_hint() -> int        (0 = no limit; lowered automatically after an out-of-memory error)
    export_text(model_dir, model_name) -> str   (path of the text tower onnx, exported if missing)

The C++ side decodes and resizes images to size x size RGB (using resize_mode), so this module only
uploads uint8 pixels, normalises on the device and runs the vision tower.
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
