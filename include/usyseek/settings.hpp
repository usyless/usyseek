#pragma once

#include "common.hpp"

#include <glaze/glaze.hpp>

namespace usyseek {

struct Settings {
    // Search pill / hotkey
    bool hotkey_enabled = true;
    uint32_t hotkey_mods = MOD_ALT;
    uint32_t hotkey_vk = VK_SPACE;
    int32_t ui_idle_timeout_s = 180; // 0 = immediate exit on hide, >0 = seconds, <0 = never
    bool hide_on_focus_loss = true;
    int32_t pill_x = INT32_MIN; // physical pixels, INT32_MIN = centre of the active monitor
    int32_t pill_y = INT32_MIN;
    int32_t pill_w = 0;         // DIPs, 0 = default
    int32_t pill_h = 0;

    // Indexing scope
    bool whole_pc = true;
    std::vector<std::string> include_dirs;
    std::vector<std::string> exclude_dirs;
    bool index_images = true;
    bool index_videos = true;
    uint32_t min_file_kb = 16;
    uint32_t video_frames = 8;

    // Model / compute
    std::string model_dir;
    std::string model_name = "PE-Core-L-14-336";
    std::string device = "auto"; // auto | cpu
    uint32_t batch_size = 0;     // 0 = automatic (fits free VRAM)
    uint32_t decode_threads = 0; // 0 = automatic

    // Python environment
    std::string venv_dir;
    std::string base_python;
    std::string vision_script; // empty = bundled usyseek_vision.py
    std::string torch_backend = "rocm"; // cuda | rocm | cpu
    std::string rocm_arch = "gfx1030";

    // Database
    uint16_t qdrant_http_port = 6389;
    uint16_t qdrant_grpc_port = 6390;
};

/// Written by the indexer only.
struct IndexState {
    int64_t last_index_unix = 0;
    uint64_t last_index_found = 0;
    std::string last_error;
};

inline constexpr auto JSON_READ = glz::opts{.error_on_unknown_keys = false};
inline constexpr auto JSON_WRITE = glz::opts{.prettify = true};

template <typename T>
inline T load_json_file(const fs::path& p) {
    T value{};
    if (auto bytes = ulp::fs::read_as_bytes(p)) {
        (void)glz::read<JSON_READ>(value, *bytes);
    }
    return value;
}

template <typename T>
inline bool save_json_file(const fs::path& p, const T& value) {
    auto out = glz::write<JSON_WRITE>(value);
    if (!out) return false;
    const fs::path tmp = p.wstring() + L".tmp";
    if (!ulp::fs::write(tmp, std::string_view{*out})) return false;
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

/// Exclusions that can never be removed (shown with a lock in settings).
inline std::vector<std::wstring> locked_excludes() {
    std::vector<std::wstring> v;
    std::wstring win = known_folder(FOLDERID_Windows);
    if (win.empty()) win = L"C:\\Windows";
    v.push_back(win);
    const std::wstring drive = win.substr(0, 2);
    v.push_back(drive + L"\\$Recycle.Bin");
    v.push_back(drive + L"\\System Volume Information");
    v.push_back(data_dir().wstring());
    return v;
}

inline std::vector<std::string> default_excludes() {
    std::vector<std::string> v;
    for (auto id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramData, FOLDERID_LocalAppData}) {
        auto p = known_folder(id);
        if (!p.empty()) v.push_back(to_utf8(p));
    }
    v.push_back("*\\node_modules");
    v.push_back("*\\.git");
    v.push_back("*\\site-packages");
    return v;
}

inline Settings load_settings() {
    std::error_code ec;
    const bool existed = fs::exists(settings_path(), ec);
    Settings s = load_json_file<Settings>(settings_path());
    if (!existed) {
        s.exclude_dirs = default_excludes();
    }
    if (s.model_dir.empty()) {
        if (auto p = find_upwards(L"usybooru\\clip-model")) s.model_dir = to_utf8(p->wstring());
        else s.model_dir = to_utf8((data_dir() / L"models").wstring());
    }
    if (s.venv_dir.empty()) {
        const fs::path own = data_dir() / L"venv";
        if (fs::exists(own / L"pyvenv.cfg", ec)) s.venv_dir = to_utf8(own.wstring());
        else if (auto p = find_upwards(L"usybooru\\venv_torch_native_new")) s.venv_dir = to_utf8(p->wstring());
        else s.venv_dir = to_utf8(own.wstring());
    }
    if (!existed) save_json_file(settings_path(), s);
    return s;
}

inline bool save_settings(const Settings& s) {
    return save_json_file(settings_path(), s);
}

// ---------------------------------------------------------------------------------------------
// Python virtual environment inspection (pyvenv.cfg)
// ---------------------------------------------------------------------------------------------

struct VenvInfo {
    bool exists = false;
    std::wstring home;        // base interpreter directory
    std::wstring version;     // e.g. 3.12.10
    std::wstring python_exe;  // venv\Scripts\python.exe
};

inline VenvInfo read_venv(const fs::path& venv) {
    VenvInfo info;
    const auto cfg = ulp::fs::read_as_bytes(venv / L"pyvenv.cfg");
    if (!cfg) return info;
    info.exists = true;
    info.python_exe = (venv / L"Scripts" / L"python.exe").wstring();
    std::string_view rest{*cfg};
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        auto key = to_wide(line.substr(0, eq));
        auto val = to_wide(line.substr(eq + 1));
        key = trim(key);
        val = trim(val);
        if (key == L"home") info.home = val;
        else if (key == L"version" || key == L"version_info") {
            if (info.version.empty() || key == L"version") info.version = val;
        }
    }
    return info;
}

/// "3.12" from "3.12.10"
inline std::wstring major_minor(std::wstring_view v) {
    const auto a = v.find(L'.');
    if (a == std::wstring_view::npos) return std::wstring{v};
    const auto b = v.find(L'.', a + 1);
    return std::wstring{v.substr(0, b)};
}

} // namespace usyseek
