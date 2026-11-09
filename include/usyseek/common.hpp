#pragma once

#include <usylibpp/usylibpp.hpp>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rapidhash.h>

namespace usyseek {

namespace fs = std::filesystem;

inline constexpr wchar_t APP_NAME[] = L"usyseek";
inline constexpr wchar_t APP_VERSION[] = L"0.1.0";

inline constexpr wchar_t LAUNCHER_CLASS[] = L"usyseek.launcher";
inline constexpr wchar_t LAUNCHER_MUTEX[] = L"Local\\usyseek.launcher";
inline constexpr wchar_t PILL_CLASS[] = L"usyseek.pill";
inline constexpr wchar_t SETTINGS_CLASS[] = L"usyseek.settings";
inline constexpr wchar_t STATUS_SHM[] = L"Local\\usyseek.indexer.status";
inline constexpr wchar_t INDEXER_STOP_EVENT[] = L"Local\\usyseek.indexer.stop";
inline constexpr wchar_t EVERYTHING_INSTANCE[] = L"uk.usyless.usyseek";

// WM_COPYDATA commands sent to the launcher window
enum class Cmd : ULONG_PTR {
    UiHello = 0x5553'0001,   // payload: uint64 pill hwnd
    StartIndex,
    StopIndex,
    ReloadSettings,
    SuspendHotkey,           // payload: uint32 (1 = suspend, 0 = resume)
    NeedQdrant,
    Quit,
};

// Registered window messages (cross-process)
inline UINT msg_toggle_search() noexcept {
    static const UINT m = RegisterWindowMessageW(L"usyseek.toggle_search");
    return m;
}
inline UINT msg_open_settings() noexcept {
    static const UINT m = RegisterWindowMessageW(L"usyseek.open_settings");
    return m;
}
inline UINT msg_indexer_changed() noexcept {
    static const UINT m = RegisterWindowMessageW(L"usyseek.indexer_changed");
    return m;
}

// ---------------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------------

inline std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::wstring lower(std::wstring_view s) {
    std::wstring out{s};
    if (!out.empty()) {
        CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

inline std::wstring trim(std::wstring_view s) {
    const auto b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring_view::npos) return {};
    const auto e = s.find_last_not_of(L" \t\r\n");
    return std::wstring{s.substr(b, e - b + 1)};
}

/// Normalises a directory for comparisons and the ancestor index: lowercase, no trailing backslash.
inline std::wstring normalise_dir(std::wstring_view d) {
    std::wstring s = lower(trim(d));
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.starts_with(L"\\\\?\\")) s.erase(0, 4);
    while (s.size() > 2 && s.back() == L'\\') s.pop_back();
    if (s.size() == 3 && s[1] == L':' && s[2] == L'\\') s.pop_back();
    return s;
}

/// Stable 64-bit point id for a file path (case-insensitive).
inline uint64_t path_id(std::wstring_view path) {
    const std::wstring l = lower(path);
    return rapidhash(l.data(), l.size() * sizeof(wchar_t));
}

inline std::wstring format_count(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

inline std::wstring format_size(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes >= (1ull << 30)) swprintf_s(buf, L"%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else if (bytes >= (1ull << 20)) swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1ull << 20));
    else if (bytes >= 1024) swprintf_s(buf, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    else swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

inline std::wstring format_duration_ms(uint64_t ms) {
    const uint64_t s = ms / 1000;
    wchar_t buf[32];
    if (s >= 3600) swprintf_s(buf, L"%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    else swprintf_s(buf, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

inline int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::wstring format_ago(int64_t unix_time) {
    if (unix_time <= 0) return L"Never";
    const int64_t d = std::max<int64_t>(0, unix_now() - unix_time);
    wchar_t buf[48];
    if (d < 60) return L"Just now";
    if (d < 3600) swprintf_s(buf, L"%lld min ago", d / 60);
    else if (d < 86400) swprintf_s(buf, L"%lld h ago", d / 3600);
    else swprintf_s(buf, L"%lld d ago", d / 86400);
    return buf;
}

inline int64_t filetime_to_unix(uint64_t ft) {
    return ft == 0 ? 0 : static_cast<int64_t>(ft / 10'000'000ull) - 11'644'473'600ll;
}

// ---------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------

inline const fs::path& exe_dir() {
    static const fs::path p = [] {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (n < buf.size()) {
                buf.resize(n);
                break;
            }
            buf.resize(buf.size() * 2);
        }
        return fs::path{buf}.parent_path();
    }();
    return p;
}

inline const fs::path& data_dir() {
    static const fs::path p = [] {
        fs::path d = exe_dir() / L"data";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return p;
}

inline fs::path settings_path() { return data_dir() / L"settings.json"; }
inline fs::path state_path() { return data_dir() / L"state.json"; }

/// Looks for a sibling project folder (e.g. usybooru) walking up from the executable directory.
inline std::optional<fs::path> find_upwards(const fs::path& relative) {
    std::error_code ec;
    for (fs::path d = exe_dir(); !d.empty(); d = d.parent_path()) {
        const fs::path c = d / relative;
        if (fs::exists(c, ec)) return c;
        if (d == d.parent_path()) break;
    }
    return std::nullopt;
}

inline std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// ---------------------------------------------------------------------------------------------
// IPC helpers
// ---------------------------------------------------------------------------------------------

inline HWND find_launcher() noexcept {
    return FindWindowW(LAUNCHER_CLASS, nullptr);
}

inline bool send_cmd(HWND target, Cmd cmd, std::span<const std::byte> payload = {}) noexcept {
    if (!target) return false;
    COPYDATASTRUCT cds{};
    cds.dwData = static_cast<ULONG_PTR>(cmd);
    cds.cbData = static_cast<DWORD>(payload.size());
    cds.lpData = payload.empty() ? nullptr : const_cast<std::byte*>(payload.data());
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 2000, &result) != 0;
}

template <typename T>
inline bool send_cmd_value(HWND target, Cmd cmd, const T& value) noexcept {
    return send_cmd(target, cmd, std::as_bytes(std::span{&value, 1}));
}

inline bool send_to_launcher(Cmd cmd) noexcept {
    return send_cmd(find_launcher(), cmd);
}

/// Writes a line to data/<name>.log (best-effort, append).
inline void log_line(const wchar_t* name, std::string_view line) {
    const fs::path p = data_dir() / (std::wstring{name} + L".log");
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

} // namespace usyseek
