#pragma once

#include "common.hpp"

#include <charconv>
#include <memory>

#include <glaze/glaze.hpp>

namespace usyseek {

enum class ThumbDblClick : uint8_t { Open, Preview };

[[nodiscard]] inline ThumbDblClick parse_thumb_dblclick(std::string_view s) noexcept {
    return s == "preview" ? ThumbDblClick::Preview : ThumbDblClick::Open;
}

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
    std::string thumb_fit = "crop"; // crop | contain
    std::string thumb_dblclick = "open"; // open | preview
    std::string theme = "system"; // system | accent | dark | light | nord | mocha | oled | custom:<name>
    double grid_zoom = 1.0; // >= 1.0
    double video_volume = 1.0;
    bool video_muted = false;

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
    std::string python_version = "3.14"; // NuGet portable Python version (e.g. 3.14, 3.13, 3.12.2)
    std::string vision_script;           // empty = bundled usyseek_vision.py
    std::string torch_backend = "rocm";  // rocm | cuda | cpu | manual
    std::string rocm_arch = "gfx1030";
    std::string cuda_version = "cu126";  // PyTorch CUDA wheel index, e.g. cu126 | cu130
    std::string torch_pip_args;          // optional custom pip install args for torch/torchvision

    // Updates
    bool auto_check_updates = true;
};

/// Written by the indexer only.
struct IndexState {
    int64_t last_index_unix = 0;
    uint64_t last_index_found = 0;
    std::optional<uint64_t> last_index_embedded;
    std::string last_error;
    uint32_t auto_batch = 0; // batch size benchmarked for auto_batch_key, 0 = not measured
    std::string auto_batch_key;
};

inline constexpr auto JSON_READ = glz::opts{.error_on_unknown_keys = false};
inline constexpr auto JSON_WRITE = glz::opts{.prettify = true};

template <typename T>
inline T load_json_file(const fs::path& p, bool* parse_ok = nullptr) {
    T value{};
    if (parse_ok) *parse_ok = true;
    if (auto bytes = ulp::fs::read_as_bytes(p)) {
        const auto err = glz::read<JSON_READ>(value, *bytes);
        if (err && parse_ok) *parse_ok = false;
    }
    return value;
}

template <typename T>
inline bool save_json_file(const fs::path& p, const T& value) {
    auto out = glz::write<JSON_WRITE>(value);
    if (!out) return false;
    const fs::path tmp = p.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId());
    if (!ulp::fs::write(tmp, std::string_view{*out})) return false;
    if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) return true;
    const DWORD err = GetLastError();
    std::error_code ec;
    fs::remove(tmp, ec);
    SetLastError(err);
    return false;
}

/// Exclusions that can never be removed (shown with a lock in settings).
inline const std::vector<std::wstring>& locked_excludes() {
    static const std::vector<std::wstring> v = [] {
        std::vector<std::wstring> out;
        out.reserve(4);
        std::wstring win = known_folder(FOLDERID_Windows);
        if (win.empty()) win = L"C:\\Windows";
        const std::wstring drive = win.substr(0, 2);
        out.push_back(std::move(win));
        out.push_back(drive + L"\\$Recycle.Bin");
        out.push_back(drive + L"\\System Volume Information");
        out.push_back(data_dir().wstring());
        return out;
    }();
    return v;
}

inline std::vector<std::string> default_excludes() {
    std::vector<std::string> v;
    v.reserve(7);
    for (auto id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramData, FOLDERID_LocalAppData}) {
        auto p = known_folder(id);
        if (!p.empty()) v.push_back(to_utf8(p));
    }
    v.emplace_back("*\\node_modules");
    v.emplace_back("*\\.git");
    v.emplace_back("*\\site-packages");
    return v;
}

inline bool is_startup_enabled() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    std::array<wchar_t, MAX_PATH * 2> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    buf[0] = L'\0';
    auto sz = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"usyseek", nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &sz);
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

inline Settings load_settings(bool* corrupt_recovered = nullptr) {
    std::error_code ec;
    const bool existed = fs::exists(settings_path(), ec);
    bool ok = true;
    Settings s = load_json_file<Settings>(settings_path(), &ok);
    const bool corrupted = existed && !ok;
    if (corrupt_recovered) *corrupt_recovered = corrupted;
    s.launch_on_startup = is_startup_enabled();
    if (!existed || corrupted) {
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

        // Tab-separated format: id \t mtime \t size \t path \t reason
        if (const auto t1 = line.find('\t'); t1 != std::string_view::npos) {
            FailedFile f;
            const std::string_view s_id = line.substr(0, t1);
            std::from_chars(s_id.data(), s_id.data() + s_id.size(), f.id);
            const auto r1 = line.substr(t1 + 1);
            const auto t2 = r1.find('\t');
            if (t2 == std::string_view::npos) continue;
            const std::string_view s_mtime = r1.substr(0, t2);
            std::from_chars(s_mtime.data(), s_mtime.data() + s_mtime.size(), f.mtime);
            const auto r2 = r1.substr(t2 + 1);
            const auto t3 = r2.find('\t');
            if (t3 == std::string_view::npos) continue;
            const std::string_view s_size = r2.substr(0, t3);
            std::from_chars(s_size.data(), s_size.data() + s_size.size(), f.size);
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
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Python environment inspection (.venv or standalone / NuGet portable Python)
// ---------------------------------------------------------------------------------------------

struct VenvInfo {
    bool exists = false;
    bool portable = false;      // true when python.exe lives directly in the directory (e.g. NuGet portable)
    std::wstring home;          // base interpreter directory
    std::wstring version;       // e.g. 3.14.0
    std::wstring python_exe;    // venv\Scripts\python.exe or portable\python.exe
    std::wstring site_packages; // Lib\site-packages
};

inline std::wstring read_pe_version(const fs::path& exe) {
    DWORD dummy = 0;
    const DWORD sz = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (sz == 0) return {};
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(sz); // NOLINT(modernize-avoid-c-arrays)
    if (!GetFileVersionInfoW(exe.c_str(), 0, sz, buf.get())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.get(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return {};
    return std::to_wstring(HIWORD(ffi->dwFileVersionMS)) + L"." +
           std::to_wstring(LOWORD(ffi->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(ffi->dwFileVersionLS));
}

inline VenvInfo read_venv(const fs::path& venv) {
    VenvInfo info;
    if (const auto cfg = ulp::fs::read_as_bytes(venv / L"pyvenv.cfg")) {
        info.exists = true;
        info.portable = false;
        info.python_exe = (venv / L"Scripts" / L"python.exe").wstring();
        info.site_packages = (venv / L"Lib" / L"site-packages").wstring();
        const auto trim_sv = [](std::string_view sv) noexcept {
            const auto b = sv.find_first_not_of(" \t\r\n");
            if (b == std::string_view::npos) return std::string_view{};
            const auto e = sv.find_last_not_of(" \t\r\n");
            return sv.substr(b, e - b + 1);
        };
        std::string_view rest{*cfg};
        while (!rest.empty()) {
            const auto nl = rest.find('\n');
            std::string_view line = rest.substr(0, nl);
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            const std::string_view key = trim_sv(line.substr(0, eq));
            const std::string_view val = trim_sv(line.substr(eq + 1));
            if (key == "home") info.home = to_wide(val);
            else if (key == "version" || key == "version_info") {
                if (info.version.empty() || key == "version") info.version = to_wide(val);
            }
        }
        return info;
    }

    // Portable / standalone Python directory (e.g. NuGet python package or WinPython)
    std::error_code ec;
    fs::path root = venv;
    if (!fs::exists(root / L"python.exe", ec) && fs::exists(root / L"tools" / L"python.exe", ec)) {
        root = root / L"tools";
    }
    const fs::path py_exe = root / L"python.exe";
    if (fs::exists(py_exe, ec)) {
        info.exists = true;
        info.portable = true;
        info.home = root.wstring();
        info.python_exe = py_exe.wstring();
        info.site_packages = (root / L"Lib" / L"site-packages").wstring();
        info.version = read_pe_version(py_exe);
        if (info.version.empty()) info.version = L"3.x";
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
