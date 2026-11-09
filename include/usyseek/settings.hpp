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
    bool launch_on_startup = false;
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
    std::string vision_script;          // empty = bundled usyseek_vision.py
    std::string torch_backend = "rocm"; // rocm | cuda | cpu | manual
    std::string rocm_arch = "gfx1030";
    std::string torch_pip_args;         // optional custom pip install args for torch/torchvision

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

inline bool is_startup_enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[MAX_PATH * 2]{};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"usyseek", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != L'\0';
}

inline bool set_startup_enabled(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st = ERROR_SUCCESS;
    if (enable) {
        const fs::path exe = exe_dir() / L"usyseek.exe";
        const std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        st = RegSetValueExW(key, L"usyseek", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, L"usyseek");
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

inline Settings load_settings() {
    std::error_code ec;
    const bool existed = fs::exists(settings_path(), ec);
    Settings s = load_json_file<Settings>(settings_path());
    s.launch_on_startup = is_startup_enabled();
    if (!existed) {
        s.exclude_dirs = default_excludes();
    }
    if (s.model_dir.empty()) {
        const fs::path models = data_dir() / L"models";
        fs::create_directories(models, ec);
        s.model_dir = to_utf8(models.wstring());
    }
    if (s.venv_dir.empty()) {
        const fs::path own = data_dir() / L"venv";
        s.venv_dir = to_utf8(own.wstring());
    }
    if (!existed) save_json_file(settings_path(), s);
    return s;
}

inline bool save_settings(const Settings& s) {
    return save_json_file(settings_path(), s);
}

// ---------------------------------------------------------------------------------------------
// Failed files log (shared between indexer and UI)
// ---------------------------------------------------------------------------------------------

struct FailedFile {
    uint64_t id = 0;
    int64_t mtime = 0;
    uint64_t size = 0;
    std::wstring path;
    std::wstring reason;
};

inline fs::path failed_path(std::string_view coll) {
    return data_dir() / (L"failed-" + to_wide(coll) + L".txt");
}

inline std::vector<FailedFile> load_failed_files(std::string_view coll) {
    std::vector<FailedFile> out;
    const auto bytes = ulp::fs::read_as_bytes(failed_path(coll));
    if (!bytes) return out;
    std::string_view rest{*bytes};
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;

        FailedFile f;
        // Tab-separated format: id \t mtime \t size \t path \t reason
        if (const auto t1 = line.find('\t'); t1 != std::string_view::npos) {
            f.id = std::strtoull(std::string{line.substr(0, t1)}.c_str(), nullptr, 10);
            const auto r1 = line.substr(t1 + 1);
            const auto t2 = r1.find('\t');
            if (t2 == std::string_view::npos) continue;
            f.mtime = std::strtoll(std::string{r1.substr(0, t2)}.c_str(), nullptr, 10);
            const auto r2 = r1.substr(t2 + 1);
            const auto t3 = r2.find('\t');
            if (t3 == std::string_view::npos) continue;
            f.size = std::strtoull(std::string{r2.substr(0, t3)}.c_str(), nullptr, 10);
            const auto r3 = r2.substr(t3 + 1);
            const auto t4 = r3.find('\t');
            if (t4 == std::string_view::npos) {
                f.path = to_wide(r3);
                f.reason = L"Decode failed";
            } else {
                f.path = to_wide(r3.substr(0, t4));
                f.reason = to_wide(r3.substr(t4 + 1));
            }
            out.push_back(std::move(f));
        } else {
            // Legacy space-separated format: id mtime size
            unsigned long long id = 0, sz = 0;
            long long mt = 0;
            if (std::sscanf(std::string{line}.c_str(), "%llu %lld %llu", &id, &mt, &sz) == 3) {
                f.id = id;
                f.mtime = mt;
                f.size = sz;
                f.reason = L"Decode failed";
                out.push_back(std::move(f));
            }
        }
    }
    return out;
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
