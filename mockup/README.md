# usyseek — Windows Spotlight & Semantic Media Search UI Mockup

A browser-based interactive prototype for **usyseek**, an AI-powered semantic desktop search application for Windows 11 designed with a flat **Material 3 (M3)** design system. Clean, modern, and colorful with tonal surfaces, ready for native Windows Mica backdrop material integration.

---

## 🚀 How to Run the Mockup

You can open and interact with the mockup in three ways:

1. **Directly in your Browser**:
   - Double-click [`index.html`](file:///mnt/c/Users/usy/Documents/Projects/usyseek/index.html) (or right click -> Open With -> Microsoft Edge / Google Chrome / Firefox).
2. **Via Batch Launcher**:
   - Double-click [`run_mockup.bat`](file:///mnt/c/Users/usy/Documents/Projects/usyseek/run_mockup.bat).
3. **Via Local Python Server**:
   ```bash
   python serve.py
   ```
   and navigate to `http://localhost:5173/index.html`.

---

## ✨ Design System: Material 3 Flat (Native Mica Ready)

- **Flat Tonal Surfaces**: Built without CSS gradients, radial glow filters, or artificial blur overlays. All surfaces use solid Material 3 tonal elevation tokens (`--md-sys-color-surface-container`, `--md-sys-color-surface-container-high`, etc.). When porting to native Windows (WinUI 3 / App SDK / Tauri / Electron), native Mica or Acrylic backdrops can be cleanly applied directly behind these tonal containers.
- **Modern & Colorful**: Employs Material 3 dynamic color tokens (Primary Sky Blue `#a8c7fa`, Soft Mint `#70d6b6`, Warm Amber `#ffb951`, Peach `#ffb4ab`, Lavender `#d0bcff`) for crisp, accessible contrast.
- **Crisp Flat Vector Illustrations**: All media thumbnails feature clean geometric vector graphics with flat color planes, sharp silhouettes, and bold architectural / natural compositions—no gradient fills or cheesy blur effects.

---

## 🔍 Features Implemented in the Prototype

### 1. Spotlight Search Pill & Media Grid
- **Draggable & Movable**: Click and drag anywhere along the search pill bar to move the spotlight search freely around your screen (positions automatically adapt to your desktop).
- **User Width Resizable**:
  - Drag the left and right edges with live pixel size tooltips.
  - Double-click an edge to snap back to default width (780px).
- **Fullscreen Mode**:
  - Click the **Fullscreen** icon in the search pill (or press Escape to exit) to expand into an immersive, full-screen media exploration grid.
  - In fullscreen mode, the media viewport smoothly stretches edge-to-edge.
- **Media Results & Tagging**:
  - **Uniform Media Previews**: Every card in the grid has an identical 16:10 thumbnail preview and total card height.
  - **Pinned Bottom Metadata**: Resolution and file size (`3840×2160 • 14.2 MB`) and tagging status are pinned strictly to the bottom of each card with `margin-top: auto` and a clean top divider, forming a uniform baseline across every row.
  - **Subtle Similarity Button**: A small circular icon in the thumbnail corner replaces large hover overlays.
  - **Color-Coordinated Tags**: Single-line tag strip with color-coordinated M3 assist chips and `+N` badge (hover shows all tags in tooltip). Clicking a tag filters the library.
  - **Click-to-Tag Modal**: Clicking any media card opens an M3 Tag Editor dialog to view applied tags, remove tags, pick from an 8-color tonal palette, or create custom tags.
  - Shows **Last indexed time** (e.g. `Last indexed: Just now` / `Scanning live...`).
  - Category filters: `All`, `Images`, `Videos`.

### 2. Overall Tagging Menu & Autocomplete
- **🏷️ Tagging Menu (Stamp Mode)**:
  - Click the `🏷️` button on the search pill bar to reveal the Tag Ribbon.
  - Select any tag to enter **Stamp Mode**: simply click images in the grid to immediately add or remove that tag with one click!
  - Create new tags with custom colors directly from the ribbon.
- **`tag:<tag>` Autocomplete Search**:
  - Type `tag:` into the search input to trigger an autocomplete dropdown showing matching color dots, tag names, and item counts.
  - Pressing Enter or clicking a suggestion filters down to items with that tag.
- **Directory Path Search**:
  - Type any folder name (e.g. `Wallpapers`, `Photography`, `Drone`, `Clips`) or directory path (e.g. `C:\Users\usy\Pictures`, `dir:...`) to instantly show only files located within that folder.
  - Autocomplete suggests existing media folders as you type.
- **"Find Similar" Feature**:
  - Every media card includes a **"Similar"** button.
  - Clicking it instantly puts `similar:<file path>` into the search box and ranks the library by visual / semantic cosine similarity with confidence percentage badges (e.g. `98.4% match`).

### 3. Settings & Embeddings Window
- **Draggable Window**: Material 3 window with title bar, minimize, maximize, and close buttons.
- **Tab 1: Embeddings & Progress**:
  - **Hero Stats**: Displays `embedded files / total files found on PC` (`14,820 / 48,200`).
  - **Start / Stop Indexing**: Toggle button with active background file ticker and throughput rate (`~38 files/sec`).
  - **Indexing Scope**: Switch between *Include the whole PC (all local drives C:\, D:\)* and *Specify directories to include* with dynamic list and `+ Add Directory` action.
  - **Excluded Directories Section (Always Visible)**:
    - Pre-configured system rules (`C:\Windows`, `C:\Program Files`).
    - Custom patterns (`**\node_modules\**`, `**\.git\**`, `C:\Users\usy\AppData`).
    - Ability to add new excluded paths or remove custom rules.
- **Tab 2: Application & Venv Settings**:
  - **Global Keybind Control**:
    - Current shortcut preview: `<kbd>Alt</kbd> + <kbd>Space</kbd>`.
    - **Interactive Keybind Recording**: Click "Record New Keybind" and press any key combination (e.g. `Ctrl+Shift+Space`).
    - **Enable / Disable Toggle Switch**: Turn the global shortcut ON or OFF.
  - **Embeddings AI Model Selector**:
    - Switch between `CLIP-ViT-B/32`, `SigLIP-SO400M`, `OpenCLIP-ViT-L/14`, `Nomic-Embed-Vision`, or custom path.
    - Shows live vector dimensions (512-d, 768-d, 1152-d), VRAM requirements, and device (`CUDA RTX 4070`).
  - **Python Virtualenv (.venv) Management**:
    - Path display: `C:\Users\usy\Documents\Projects\usyseek\.venv`.
    - **Open Command Line in .venv**: Launches a simulated Windows Terminal with prompt `(.venv) C:\Users\usy\Documents\Projects\usyseek>`. Supports mock commands (`help`, `pip list`, `python -V`, `python index.py --stats`, `cls`).
    - **Update Venv**: Simulates `pip install --upgrade -r requirements.txt`.
    - **Recreate Venv**: Rebuilds virtual environment from scratch.
    - **Delete Venv**: Soft deletion with status badge update.

### 4. System Tray & Win32 Context Menu
- **Taskbar System Tray Icon**:
  - Displays indexing status with an animated pulse dot.
  - **Left-Click**: Toggles the Spotlight search pill open/closed.
  - **Right-Click**: Opens a Win32 dark context menu with options:
    - 🔍 *Open Search* (`Alt+Space`)
    - ⚙️ *Open Interface (Settings)*
    - ⚡ *Start / Stop Index*
    - ❌ *Quit usyseek*

---

## ⌨️ Interactive Shortcuts
- `Alt + Space` (or your recorded keybind): Toggle Spotlight Search Pill.
- `Esc`: Close context menus, dismiss the pill, or exit fullscreen.
- `Double click handle`: Reset search bar width.
