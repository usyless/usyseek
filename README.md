<h1 align="center">usyseek</h1>
<h3 align="center">Local AI-Powered Semantic Desktop Search for Windows</h3>

> [!IMPORTANT]
> **Notice: This entire application, architecture, and codebase is 100% AI-generated.**

- **100% On-Device & Privacy-First**: All scanning, decoding, embedding, vector storage, and search inference run locally. Zero telemetry, zero analytics, zero cloud dependencies.
- **No System-Wide Python Dependency**: System Python is completely optional. A self-contained portable Python runtime can be downloaded and managed directly from the Settings UI with one click.
- **Found a bug or have a suggestion?** Feel free to open an issue or reach out to `@usy_` on Discord!
- **Contributions are welcome**: PRs and improvements will always be considered!

---

## Overview

**usyseek** is a modern, high-performance semantic desktop search engine for Windows 10/11 built with **C++23**, **Rust**, **Direct2D**, and **PyTorch**.

Instead of relying solely on file names or manually tagged metadata, `usyseek` allows you to search your entire local image and video library using natural language descriptions (e.g., *"sunset over snowy mountain"*, *"cyberpunk street in the rain"*, *"diagram with purple flowchart"*), visual similarity (*"Find Similar"*), directory path scoping, and interactive color-coded tags.

Designed around a decoupled multi-process architecture, `usyseek` delivers instant Spotlight search capabilities while maintaining a virtually zero-byte idle memory footprint.

---

## Architecture & Process Model

To avoid the massive idle resource consumption common to AI and Electron-based desktop utilities, `usyseek` splits responsibilities across dedicated processes coordinated via Windows Job Objects, shared memory, and window messages:

```
                  ┌────────────────────────────────────────┐
                  │              usyseek.exe               │
                  │   (Lightweight Launcher & Tray Icon)   │
                  │   Global Hotkey • Process Supervision  │
                  └───────┬────────────────────────┬───────┘
                          │ (on hotkey / search)   │ (on index start)
                          ▼                        ▼
        ┌───────────────────────────┐    ┌───────────────────────────┐
        │       usyseek-ui.exe      │    │    usyseek-indexer.exe    │
        │  • Direct2D / DirectWrite │    │  • Voidtools Everything   │
        │  • Spotlight Search Pill  │    │  • WIC / Media Foundation │
        │  • ONNX Runtime Text Tower│    │  • Zero-copy Shared Memory│
        │  • Exits on idle timeout  │    │  • Exits when scan finishes│
        └─────────────┬─────────────┘    └─────────────┬─────────────┘
                      │                                │ Shared Memory IPC
                      │                                ▼
                      │                  ┌───────────────────────────┐
                      │                  │    usyseek_vision.py      │
                      │                  │  • PyTorch Vision Tower   │
                      │                  │  • ROCm / CUDA / CPU      │
                      │                  │  • Portable Python Worker │
                      │                  └─────────────┬─────────────┘
                      │ gRPC (Rust staticlib)          │ gRPC (Rust staticlib)
                      ▼                                ▼
        ┌────────────────────────────────────────────────────────────┐
        │                         qdrant.exe                         │
        │        Local Vector Database (Embeddings & Payloads)       │
        └────────────────────────────────────────────────────────────┘
```

1. **`usyseek.exe` (Launcher & Supervisor)**:
   - Always-running background supervisor (~3–5 MB working set, trimmed on idle).
   - Manages the system tray icon, global hotkey (default: `Alt + Space`), and process lifecycles.
   - Enforces a `KillOnClose` Windows Job object so background helpers never leak when quitting.

2. **`usyseek-ui.exe` (Search Pill & Settings Window)**:
   - Hardware-accelerated UI rendered with **Direct2D**, **DirectWrite**, **DirectComposition**, and **DXGI**.
   - Features a floating Spotlight search pill, fullscreen media exploration grid, tag editor, and settings interface following a flat **Material 3 (M3)** design system.
   - Runs **ONNX Runtime (CPU)** to encode text queries on-the-fly into vector embeddings using the exported text tower and fast Hugging Face tokenizers.
   - **Zero-Footprint Idle**: After `ui_idle_timeout_s` of inactivity while hidden (default: 180 seconds), the UI process exits cleanly, releasing all GUI and ONNX resources back to the OS.

3. **`usyseek-indexer.exe` (Scanner & Embedding Pipeline)**:
   - On-demand worker process that exists only while indexing is active.
   - Embeds an isolated instance of **Voidtools Everything** (`uk.usyless.usyseek`) for instant NTFS file discovery across local drives.
   - Utilizes **Windows Imaging Component (WIC)** for high-throughput image decoding and **Media Foundation (MF)** for multi-threaded video keyframe extraction.
   - **Zero C++ Python Dependency**: Completely decoupled from CPython at compile and link time (no `pybind11` or `python3XY.dll` link requirement). Communicates with an out-of-process Python worker (`usyseek_vision.py`) via zero-copy **named shared memory**, executing the vision tower on AMD (ROCm), NVIDIA (CUDA), or CPU.
   - Reports live progress, throughput, and status through shared memory (`Local\usyseek.indexer.status`).

4. **`qdrant.exe` (Embedded Vector Engine)**:
   - Bundled Windows x64 Qdrant vector database running locally over gRPC and HTTP.
   - Communicates with C++ through a specialized static library written in Rust (`usyseek_rust`) wrapping `qdrant-client` and `tokio`.
   - Includes the bundled static **Qdrant Web UI** for browsing collections and vector payloads.

---

## Features

### 🔍 Spotlight Search Pill & Media Grid
- **Movable & Draggable**: Freely drag the search pill anywhere on your desktop; screen positions are preserved across launches.
- **Live Edge Resizing**: Drag left and right pill edges to adjust width with real-time pixel tooltips, or double-click to snap back to default.
- **Fullscreen Mode**: Expand instantly from the pill into an immersive edge-to-edge media exploration view.
- **Uniform 16:10 Previews**: Crisp, uniform aspect ratio media cards with pinned bottom metadata (dimensions, file size, duration, and tags).
- **Find Similar (Visual Cosine Search)**: Click the "Similar" button on any image or video card to search your library for visually and semantically related items.
- **Directory Path Scoping**: Filter searches to specific folders or parent directories using `dir:...` or path autocompletion.
- **Media Kind Filters**: Filter seamlessly between All, Images, or Videos.

### 🏷️ Interactive Tagging System
- **Single-Line Tag Strip**: Color-coordinated M3 assist chips with `+N` overflow badges and hover tooltips.
- **Stamp Mode**: Toggle the Tag Ribbon to enter stamp mode—click any card in the grid to immediately add or remove the selected tag.
- **`tag:<name>` Autocomplete**: Type `tag:` into the search box for real-time autocomplete suggestions with color dots and live item counts.
- **Custom Tag Palette**: Create custom tags with an 8-color tonal Material 3 palette.

### ⚡ Blazing Fast Indexing
- **Voidtools Everything Integration**: Near-instantaneous filesystem crawling across whole PC drives (`C:\`, `D:\`, etc.) or selected directories.
- **Multi-Threaded Hardware Decoding**: Concurrent WIC image decoding and Media Foundation video frame sampling.
- **Configurable Video Keyframes**: Extracts configurable representative frames across videos (default: 8 frames) for semantic video retrieval.
- **Robust Exclusion Rules**: Pre-configured system rules (`C:\Windows`, `$Recycle.Bin`, `Program Files`, `AppData`) plus custom glob/path exclusions.

### ⚙️ Settings & Environment Management
- **Interactive Global Hotkey Recorder**: Easily rebind the spotlight shortcut (e.g. `Alt + Space`, `Ctrl + Shift + Space`) or disable it entirely.
- **AI Model Selection**: Supports OpenCLIP and Perception Engine vision models (default: `PE-Core-L-14-336`). Automatically exports ONNX text towers for CPU search.
- **Zero-Setup Portable Python**: System-wide Python is **not required**. `usyseek` can download and bootstrap a self-contained portable Python runtime (via NuGet, supporting 3.14, 3.13, 3.12, 3.11) with one click from Settings, or use an existing `.venv`.
- **Hardware Acceleration**: Configure hardware acceleration (ROCm / CUDA / CPU) with custom PyTorch arguments directly within the UI.

---

## Shortcuts & Controls

| Shortcut / Action | Function |
| :--- | :--- |
| <kbd>Alt</kbd> + <kbd>Space</kbd> | Toggle Spotlight Search Pill (customizable in settings) |
| <kbd>Esc</kbd> | Dismiss search pill, close settings window, or exit fullscreen |
| **Drag Top/Handle** | Move the Spotlight search pill around the desktop |
| **Drag Side Edges** | Resize search pill width |
| **Double-Click Edge** | Reset search pill to default width (780px) |
| **Left-Click Tray Icon** | Toggle Spotlight Search Pill |
| **Right-Click Tray Icon** | Open Win32 context menu (Search, Settings, Start/Stop Index, Quit) |

---

## System Requirements

- **Operating System**: Windows 10 (version 1809+) or Windows 11 x64
- **C++ Compiler**: MSVC from Visual Studio 2022 (v143+) supporting C++23 (`/std:c++latest`)
- **CMake**: `3.31+`
- **Build System**: [Ninja](https://github.com/ninja-build/ninja)
- **Rust Toolchain**: `cargo` / `rustc` (used to compile the `usyseek_rust` static library)
- **Python**: **Not required!** System-wide Python is no longer a dependency. The application can automatically download and manage a portable Python runtime directly from the Settings UI (or you can optionally point it to an existing virtualenv / local install).
- **GPU (Optional but recommended for indexing)**:
  - **AMD**: ROCm for Windows (default target: `gfx1030` / RDNA2+)
  - **NVIDIA**: CUDA 11.8+ / 12.x
  - **CPU**: Supported for both indexing and search fallback

---

## Building from Source

### 1. Environment Setup

Ensure MSVC, CMake, Ninja, and Rust (`cargo`) are available in your `PATH`. Python is **not needed** to build `usyseek`. If you use portable development environments, you can configure them via `build.bat` or load your environment scripts.

### 2. Build via `build.bat`

A convenience script is provided in the repository root:

```cmd
build.bat
```

To pass additional CMake build options (such as clean rebuild or specific targets):

```cmd
build.bat --clean-first
```

### 3. Manual CMake Build

Alternatively, configure and build using CMake directly:

```cmd
cmake -S . -B build -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build --parallel
```

### 4. Build Output Layout

All built executables, dependencies, and runtime assets are automatically assembled in `build/bin/`:

```
build/bin/
├── usyseek.exe              # Launcher and tray supervisor
├── usyseek-ui.exe           # Search pill & settings UI
├── usyseek-indexer.exe      # Indexing and embedding engine
├── onnxruntime.dll          # ONNX Runtime (CPU)
├── qdrant/
│   ├── qdrant.exe           # Qdrant vector database server
│   └── static/              # Qdrant Web UI static assets
├── python/
│   ├── usyseek_vision.py    # PyTorch vision tower worker
│   └── requirements.txt     # Python dependencies
└── config/
    └── qdrant.yaml          # Qdrant configuration
```

---

## CMake Configuration Flags

| Option | Default | Description |
| :--- | :--- | :--- |
| `USYSEEK_USE_MIMALLOC` | `ON` | Replace the default allocator with Microsoft `mimalloc` |
| `USYSEEK_USE_INTERPROCEDURAL_OPTIMIZATION` | `ON` | Enable Link-Time Optimization (LTO/LTCG) |
| `USYSEEK_USE_STATIC_MSVCRT` | `ON` | Link against static MSVC runtime (`/MT`) |
| `USYSEEK_USYLIBPP_VERSION` | `main` | Git branch/tag for `usylibpp` |
| `USYSEEK_GLAZE_VERSION` | `v9.1.0` | Git tag for `glaze` (high-performance JSON) |
| `USYSEEK_MIMALLOC_VERSION` | `v3.5.3` | Git tag for `mimalloc` |
| `USYSEEK_ORT_VERSION` | `1.31.1` | Prebuilt ONNX Runtime Windows x64 release |
| `USYSEEK_QDRANT_VERSION` | `v1.20.0` | Prebuilt Qdrant Windows x64 release |
| `USYSEEK_QDRANT_WEB_UI_VERSION` | `v0.2.20`| Prebuilt Qdrant Web UI release |
| `USYSEEK_EVERYTHING_VERSION` | `1.5.0.1424b` | Voidtools Everything x64 portable release |

---

## Interactive UI Mockup

An interactive, browser-based prototype of the Material 3 search pill and settings interface is included under [`mockup/`](./mockup):

- Double-click [`mockup/index.html`](./mockup/index.html) or run [`mockup/run_mockup.bat`](./mockup/run_mockup.bat)
- Or start a local server:
  ```bash
  python mockup/serve.py
  ```
  and visit `http://localhost:5173/index.html`.

---

## Getting Help & Community

- **Found a bug?** Open an issue on GitHub.
- **Questions or ideas?** Reach out to `@usy_` on Discord.
- Pull requests and contributions are always welcome!
