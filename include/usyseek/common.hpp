#pragma once

#include <usylibpp/usylibpp.hpp>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <rapidhash.h>
#include <usyseek_version.h>

namespace usyseek {

namespace fs = std::filesystem;

template <typename T, typename... Args>
[[nodiscard]] constexpr std::array<T, sizeof...(Args)> make_array(Args&&... args) {
    return {T{std::forward<Args>(args)}...};
}

inline constexpr const wchar_t* APP_NAME = L"usyseek";
inline constexpr const wchar_t* APP_VERSION = USYSEEK_VERSION_WSTR;

inline constexpr const wchar_t* LAUNCHER_CLASS = L"usyseek.launcher";
inline constexpr const wchar_t* LAUNCHER_MUTEX = L"Local\\usyseek.launcher";
inline constexpr const wchar_t* PILL_CLASS = L"usyseek.pill";
inline constexpr const wchar_t* SETTINGS_CLASS = L"usyseek.settings";
inline constexpr const wchar_t* STATUS_SHM = L"Local\\usyseek.indexer.status";
inline constexpr const wchar_t* INDEXER_MUTEX = L"Local\\usyseek.indexer";
inline constexpr const wchar_t* INDEXER_STOP_EVENT = L"Local\\usyseek.indexer.stop";
inline constexpr const wchar_t* INDEXER_DB_WANT_EVENT = L"Local\\usyseek.db.want";
inline constexpr const wchar_t* NOTIFICATIONS_MUTEX = L"Local\\usyseek.notifications";
inline constexpr const wchar_t* EVERYTHING_INSTANCE = L"uk.usyless.usyseek";

// WM_COPYDATA commands sent to the launcher window
enum class Cmd : uint32_t {
    UiHello = 0x5553'0001,   // payload: uint64 pill hwnd
    StartIndex,
    StopIndex,
    ReloadSettings,
    SuspendHotkey,           // payload: uint32 (1 = suspend, 0 = resume)
    Quit,
    HotkeyStatus,            // query: the reply is a HotkeyState
    TestNotification,
    ShowUpdateBalloon,       // payload: UTF-8 version tag (e.g. "v0.3.0")
};

/// Whether the launcher currently holds the global search shortcut.
enum class HotkeyState : uint8_t {
    Unknown = 0, // no launcher to ask
    Registered,
    Disabled,    // switched off in settings
    Unavailable, // another application owns the combination
};

// Registered window messages (cross-process)
enum class ToggleMode : uint8_t {
    Toggle = 0,     // hotkey or direct invocation: hide if active, otherwise show
    Hide = 1,       // hide if visible (tray click started while pill was open)
    TraySelect = 2, // tray icon click fallback: avoid re-opening right after focus-loss hide
};

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
inline UINT msg_quit_ui() noexcept {
    static const UINT m = RegisterWindowMessageW(L"usyseek.quit_ui");
    return m;
}
inline UINT msg_notifications_changed() noexcept {
    static const UINT m = RegisterWindowMessageW(L"usyseek.notifications_changed");
    return m;
}
inline UINT msg_open_notifications() noexcept {
    static const UINT m = RegisterWindowMessageW(L"usyseek.open_notifications");
    return m;
}

// ---------------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------------

inline std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out;
    out.resize_and_overwrite(static_cast<size_t>(n), [&](char* buf, size_t) noexcept {
        const int written = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), buf, n, nullptr, nullptr);
        return written > 0 ? static_cast<size_t>(written) : 0u;
    });
    return out;
}

inline std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out;
    out.resize_and_overwrite(static_cast<size_t>(n), [&](wchar_t* buf, size_t) noexcept {
        const int written = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), buf, n);
        return written > 0 ? static_cast<size_t>(written) : 0u;
    });
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

/// Fixed-capacity stack string with implicit `std::basic_string_view` conversion and zero heap allocations.
template <typename Char, size_t N>
struct FixedBasicString {
    static_assert(N > 0);
    std::array<Char, N> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t len = 0;

    constexpr FixedBasicString() noexcept {
        if consteval { buf.fill(Char{}); }
        buf[0] = Char{};
    }
    constexpr FixedBasicString(const FixedBasicString& other) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(other.view());
    }
    constexpr FixedBasicString& operator=(const FixedBasicString& other) noexcept {
        if (this != &other) assign(other.view());
        return *this;
    }
    constexpr FixedBasicString(FixedBasicString&& other) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(other.view());
    }
    constexpr FixedBasicString& operator=(FixedBasicString&& other) noexcept {
        if (this != &other) assign(other.view());
        return *this;
    }
    constexpr ~FixedBasicString() noexcept = default;
    constexpr FixedBasicString(std::basic_string_view<Char> sv) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(sv);
    }
    constexpr FixedBasicString(const Char* s) noexcept
        : FixedBasicString(s ? std::basic_string_view<Char>{s} : std::basic_string_view<Char>{}) {}
    template <size_t M>
    constexpr FixedBasicString(const FixedBasicString<Char, M>& other) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(other.view());
    }

    constexpr void clear() noexcept {
        len = 0;
        buf[0] = Char{};
    }

    constexpr void assign(std::basic_string_view<Char> sv) noexcept {
        const size_t n = std::min(sv.size(), N - 1);
        for (size_t i = 0; i < n; ++i) buf[i] = sv[i];
        buf[n] = Char{};
        len = n;
    }

    constexpr void push_back(Char c) noexcept {
        if (len + 1 < N) {
            buf[len++] = c;
            buf[len] = Char{};
        }
    }

    constexpr void append(std::basic_string_view<Char> sv) noexcept {
        const size_t room = (N - 1) - len;
        const size_t n = std::min(sv.size(), room);
        for (size_t i = 0; i < n; ++i) buf[len + i] = sv[i];
        len += n;
        buf[len] = Char{};
    }

    [[nodiscard]] constexpr const Char* data() const noexcept { return buf.data(); }
    [[nodiscard]] constexpr Char* data() noexcept { return buf.data(); }
    [[nodiscard]] constexpr const Char* c_str() const noexcept { return buf.data(); }
    [[nodiscard]] constexpr size_t size() const noexcept { return len; }
    [[nodiscard]] constexpr size_t length() const noexcept { return len; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return N - 1; }
    [[nodiscard]] constexpr bool empty() const noexcept { return len == 0; }
    [[nodiscard]] constexpr const Char* begin() const noexcept { return buf.data(); }
    [[nodiscard]] constexpr const Char* end() const noexcept { return buf.data() + len; }
    [[nodiscard]] constexpr Char operator[](size_t i) const noexcept { return buf[i]; }
    [[nodiscard]] constexpr std::basic_string_view<Char> view() const noexcept { return {buf.data(), len}; }
    [[nodiscard]] constexpr operator std::basic_string_view<Char>() const noexcept { return view(); }
    [[nodiscard]] std::basic_string<Char> str() const { return std::basic_string<Char>{buf.data(), len}; }

    friend constexpr bool operator==(const FixedBasicString& a, std::basic_string_view<Char> b) noexcept {
        return a.view() == b;
    }
    friend constexpr auto operator<=>(const FixedBasicString& a, std::basic_string_view<Char> b) noexcept {
        return a.view() <=> b;
    }
};

template <size_t N>
using FixedString = FixedBasicString<char, N>;

template <size_t N>
using FixedWString = FixedBasicString<wchar_t, N>;

/// Appends `parts...` onto `dst` in one capacity check without creating intermediate strings.
template <typename Char, typename... Ts>
constexpr void append_to(std::basic_string<Char>& dst, const Ts&... parts) {
    if constexpr (sizeof...(Ts) == 0) {
        return;
    } else {
        const std::array<std::basic_string_view<Char>, sizeof...(Ts)> views{std::basic_string_view<Char>{parts}...};
        size_t add = 0;
        for (const auto sv : views) add += sv.size();
        if (add == 0) return;

        const size_t old = dst.size();
        if consteval {
            std::basic_string<Char> tmp;
            tmp.reserve(old + add);
            tmp.append(dst);
            for (const auto sv : views) tmp.append(sv);
            dst = std::move(tmp);
            return;
        }

        // Snapshot views that alias `dst` before resize_and_overwrite may reallocate `dst`'s buffer.
        const Char* const old_begin = dst.data();
        const Char* const old_end = old_begin + old;
        std::array<bool, sizeof...(Ts)> self_alias{};
        std::array<size_t, sizeof...(Ts)> self_off{};
        for (size_t i = 0; i < views.size(); ++i) {
            const Char* p = views[i].data();
            if (!views[i].empty() && !std::less<const Char*>{}(p, old_begin) && std::less<const Char*>{}(p, old_end)) {
                self_alias[i] = true;
                self_off[i] = static_cast<size_t>(p - old_begin);
            }
        }

        dst.resize_and_overwrite(old + add, [&](Char* p, size_t) noexcept {
            Char* cur = p + old;
            for (size_t i = 0; i < views.size(); ++i) {
                if (!views[i].empty()) {
                    const Char* src = self_alias[i] ? (p + self_off[i]) : views[i].data();
                    std::char_traits<Char>::copy(cur, src, views[i].size());
                    cur += views[i].size();
                }
            }
            return old + add;
        });
    }
}

template <size_t N = 32, typename T>
    requires(std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>)
[[nodiscard]] inline FixedString<N> to_fixed_str(T val) noexcept {
    FixedString<N> out;
    const auto [ptr, ec] = std::to_chars(out.buf.data(), out.buf.data() + out.buf.size() - 1, val);
    out.len = (ec == std::errc{}) ? static_cast<size_t>(ptr - out.buf.data()) : 0u;
    out.buf[out.len] = '\0';
    return out;
}

template <size_t N = 48, std::floating_point T>
[[nodiscard]] inline FixedString<N> to_fixed_str(T val, int precision) noexcept {
    FixedString<N> out;
    const auto [ptr, ec] = std::to_chars(out.buf.data(), out.buf.data() + out.buf.size() - 1, val, std::chars_format::fixed, precision);
    out.len = (ec == std::errc{}) ? static_cast<size_t>(ptr - out.buf.data()) : 0u;
    out.buf[out.len] = '\0';
    return out;
}

template <size_t N = 32, typename T>
    requires(std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>)
[[nodiscard]] inline FixedWString<N> to_fixed_wstr(T val) noexcept {
    std::array<char, N> tmp; // NOLINT(cppcoreguidelines-pro-type-member-init)
    const auto [ptr, ec] = std::to_chars(tmp.data(), tmp.data() + tmp.size() - 1, val);
    FixedWString<N> out;
    if (ec == std::errc{}) {
        const auto n = static_cast<size_t>(ptr - tmp.data());
        for (size_t i = 0; i < n; ++i) out.buf[i] = static_cast<wchar_t>(tmp[i]);
        out.len = n;
        out.buf[n] = L'\0';
    }
    return out;
}

template <size_t N = 48, std::floating_point T>
[[nodiscard]] inline FixedWString<N> to_fixed_wstr(T val, int precision) noexcept {
    std::array<char, N> tmp; // NOLINT(cppcoreguidelines-pro-type-member-init)
    const auto [ptr, ec] = std::to_chars(tmp.data(), tmp.data() + tmp.size() - 1, val, std::chars_format::fixed, precision);
    FixedWString<N> out;
    if (ec == std::errc{}) {
        const auto n = static_cast<size_t>(ptr - tmp.data());
        for (size_t i = 0; i < n; ++i) out.buf[i] = static_cast<wchar_t>(tmp[i]);
        out.len = n;
        out.buf[n] = L'\0';
    }
    return out;
}

/// Formats `n` with thousands separators into `buf` (32 chars suffices for any `uint64_t`).
inline std::wstring_view format_count_to(uint64_t n, std::span<wchar_t> buf) noexcept {
    if (buf.empty()) return {};
    size_t pos = buf.size();
    unsigned group = 0;
    do {
        if (group == 3) {
            if (pos == 0) break;
            buf[--pos] = L',';
            group = 0;
        }
        if (pos == 0) break;
        buf[--pos] = static_cast<wchar_t>(L'0' + (n % 10));
        n /= 10;
        ++group;
    } while (n > 0);
    return {buf.data() + pos, buf.size() - pos};
}

inline FixedWString<32> format_count(uint64_t n) noexcept {
    std::array<wchar_t, 32> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    return FixedWString<32>{format_count_to(n, buf)};
}

inline FixedWString<32> format_size(uint64_t bytes) noexcept {
    FixedWString<32> out;
    if (bytes >= (1ull << 30u)) {
        out = to_fixed_wstr<32>(static_cast<double>(bytes) / (1ull << 30u), 1);
        out.append(L" GB");
    } else if (bytes >= (1ull << 20u)) {
        out = to_fixed_wstr<32>(static_cast<double>(bytes) / (1ull << 20u), 1);
        out.append(L" MB");
    } else if (bytes >= 1024) {
        out = to_fixed_wstr<32>(static_cast<double>(bytes) / 1024.0, 0);
        out.append(L" KB");
    } else {
        out = to_fixed_wstr<32>(bytes);
        out.append(L" B");
    }
    return out;
}

inline FixedWString<32> format_duration_ms(uint64_t ms) noexcept {
    const uint64_t s = ms / 1000;
    FixedWString<32> out;
    const auto push2 = [&](uint64_t v) noexcept {
        out.push_back(static_cast<wchar_t>(L'0' + ((v / 10) % 10)));
        out.push_back(static_cast<wchar_t>(L'0' + (v % 10)));
    };
    if (s >= 3600) {
        out.assign(to_fixed_wstr<32>(s / 3600));
        out.push_back(L':');
        push2((s / 60) % 60);
    } else {
        out.assign(to_fixed_wstr<32>(s / 60));
    }
    out.push_back(L':');
    push2(s % 60);
    return out;
}

inline int64_t unix_now() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline FixedWString<48> format_ago(int64_t unix_time) noexcept {
    if (unix_time <= 0) return FixedWString<48>{L"Never"};
    const int64_t d = std::max<int64_t>(0, unix_now() - unix_time);
    if (d < 60) return FixedWString<48>{L"Just now"};
    FixedWString<48> out;
    if (d < 3600) {
        out = to_fixed_wstr<48>(d / 60);
        out.append(L" min ago");
    } else if (d < 86400) {
        out = to_fixed_wstr<48>(d / 3600);
        out.append(L" h ago");
    } else {
        out = to_fixed_wstr<48>(d / 86400);
        out.append(L" d ago");
    }
    return out;
}

inline int64_t filetime_to_unix(uint64_t ft) noexcept {
    return ft == 0 ? 0 : static_cast<int64_t>(ft / 10'000'000ull) - 11'644'473'600ll;
}

// ---------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------

inline const fs::path& exe_dir() {
    static const fs::path p = [] {
        std::wstring buf;
        size_t cap = MAX_PATH;
        for (;;) {
            bool done = false;
            buf.resize_and_overwrite(cap, [&](wchar_t* ptr, size_t n) noexcept {
                const DWORD len = GetModuleFileNameW(nullptr, ptr, static_cast<DWORD>(n));
                if (len < n) {
                    done = true;
                    return static_cast<size_t>(len);
                }
                return n;
            });
            if (done) break;
            cap *= 2;
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

inline const fs::path& settings_path() {
    static const fs::path p = data_dir() / L"settings.json";
    return p;
}
inline const fs::path& state_path() {
    static const fs::path p = data_dir() / L"state.json";
    return p;
}
inline const fs::path& update_state_path() {
    static const fs::path p = data_dir() / L"update.json";
    return p;
}
inline const fs::path& edge_data_dir() {
    static const fs::path p = data_dir() / L"qdrant_edge";
    return p;
}
inline const fs::path& tags_db_path() {
    static const fs::path p = data_dir() / L"tags.db";
    return p;
}

/// True while an indexer process holds the indexer mutex or is being launched.
inline bool is_indexer_db_wanted() noexcept {
    if (HANDLE mx = OpenMutexW(SYNCHRONIZE, FALSE, INDEXER_MUTEX)) {
        const DWORD w = WaitForSingleObject(mx, 0);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
            ReleaseMutex(mx);
            if (w == WAIT_ABANDONED) {
                // Previous standalone indexer exited abnormally without resetting the event.
                if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, INDEXER_DB_WANT_EVENT)) {
                    ResetEvent(ev);
                    CloseHandle(ev);
                }
            }
        } else if (w == WAIT_TIMEOUT) {
            CloseHandle(mx);
            return true;
        }
        CloseHandle(mx);
    }
    if (HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, INDEXER_DB_WANT_EVENT)) {
        const bool signaled = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
        CloseHandle(ev);
        if (signaled) return true;
    }
    return false;
}

/// Looks for a relative path walking up from the executable directory.
inline std::optional<fs::path> find_upwards(const fs::path& relative) {
    std::error_code ec;
    for (fs::path d = exe_dir(); !d.empty(); d = d.parent_path()) {
        fs::path c = d / relative;
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

/// Sends a command and returns the launcher's reply, or nothing when it could not be reached.
inline std::optional<DWORD_PTR> query_cmd(HWND target, Cmd cmd) noexcept {
    if (!target) return std::nullopt;
    COPYDATASTRUCT cds{};
    cds.dwData = static_cast<ULONG_PTR>(cmd);
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 2000, &result) == 0) return std::nullopt;
    return result;
}

template <typename T>
inline bool send_cmd_value(HWND target, Cmd cmd, const T& value) noexcept {
    return send_cmd(target, cmd, std::as_bytes(std::span{&value, 1}));
}

inline bool send_to_launcher(Cmd cmd) noexcept {
    return send_cmd(find_launcher(), cmd);
}

} // namespace usyseek
