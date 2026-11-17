#pragma once

#include "notification_store.hpp"
#include "settings.hpp"

#include <wil/resource.h>

#include <mutex>

namespace usyseek {

inline const fs::path& notifications_path() {
    static const fs::path p = data_dir() / L"notifications.json";
    return p;
}

/// Formats a Win32 error code into a readable single-line message.
inline std::string win32_error_utf8(DWORD err = GetLastError()) {
    if (err == 0) return {};
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg;
    if (n > 0 && buf) {
        msg = trim(std::wstring_view{buf, n});
    }
    if (buf) LocalFree(buf);
    if (msg.empty()) return ulp::str::concat_strings("Windows error ", to_fixed_str(err));
    return ulp::str::concat_strings(to_utf8(msg), " (error ", to_fixed_str(err), ")");
}

namespace detail {

template <typename Fn>
inline auto with_notifications_lock(Fn&& fn) {
    static std::mutex in_proc_mu;
    const std::scoped_lock in_proc_lk{in_proc_mu};
    const wil::unique_mutex_nothrow mu{CreateMutexW(nullptr, FALSE, NOTIFICATIONS_MUTEX)};
    bool owned = !mu;
    if (mu) {
        const DWORD wait = WaitForSingleObject(mu.get(), 5000);
        owned = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
    }
    const auto unlock = wil::scope_exit([&] {
        if (mu && owned) ReleaseMutex(mu.get());
    });
    return fn(owned);
}

} // namespace detail

inline void post_notifications_changed() noexcept {
    if (HWND pill = FindWindowW(PILL_CLASS, nullptr)) {
        PostMessageW(pill, msg_notifications_changed(), 0, 0);
    }
}

/// Monotonic stamp `(mtime_ticks, file_size)` so polling detects out-of-process updates cheaply.
inline std::pair<int64_t, uint64_t> notifications_file_stamp() {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(notifications_path().c_str(), GetFileExInfoStandard, &fad)) {
        return {0, 0};
    }
    const auto t = static_cast<int64_t>((static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32u) | fad.ftLastWriteTime.dwLowDateTime);
    const uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32u) | fad.nFileSizeLow;
    return {t, sz};
}

inline NotificationStoreData load_notification_store() {
    return detail::with_notifications_lock([](bool /*locked*/) {
        return load_json_file<NotificationStoreData>(notifications_path());
    });
}

inline std::vector<Notification> load_notifications() {
    return load_notification_store().items;
}

/// Appends or updates a deduplicated notification on disk and wakes any running UI window.
inline std::vector<Notification> push_notification_to_disk(std::string_view severity, std::string_view source,
                                                           std::string_view title, std::string_view detail_text,
                                                           std::string_view key = {}, bool notify_ui = true) {
    bool wrote = false;
    auto store = detail::with_notifications_lock([&](bool locked) {
        auto d = load_json_file<NotificationStoreData>(notifications_path());
        if (!locked) return d;
        apply_notification(d, severity, source, title, detail_text, key, unix_now());
        wrote = save_json_file(notifications_path(), d);
        return d;
    });
    if (wrote && notify_ui) post_notifications_changed();
    return std::move(store.items);
}

inline std::vector<Notification> dismiss_notification_on_disk(uint64_t id, bool notify_ui = false) {
    bool wrote = false;
    auto store = detail::with_notifications_lock([&](bool locked) {
        auto d = load_json_file<NotificationStoreData>(notifications_path());
        if (locked && dismiss_notification(d, id)) {
            wrote = save_json_file(notifications_path(), d);
        }
        return d;
    });
    if (wrote && notify_ui) post_notifications_changed();
    return std::move(store.items);
}

inline std::vector<Notification> clear_notification_key_on_disk(std::string_view source, std::string_view key,
                                                                bool notify_ui = true) {
    bool changed = false;
    auto store = detail::with_notifications_lock([&](bool locked) {
        auto d = load_json_file<NotificationStoreData>(notifications_path());
        if (!locked) return d;
        changed = clear_notification_key(d, source, key);
        if (changed) (void)save_json_file(notifications_path(), d);
        return d;
    });
    if (changed && notify_ui) post_notifications_changed();
    return std::move(store.items);
}

inline std::vector<Notification> clear_notifications_on_disk(bool notify_ui = false) {
    bool wrote = false;
    auto store = detail::with_notifications_lock([&](bool locked) {
        auto d = load_json_file<NotificationStoreData>(notifications_path());
        if (locked && clear_notifications(d)) {
            wrote = save_json_file(notifications_path(), d);
        }
        return d;
    });
    if (wrote && notify_ui) post_notifications_changed();
    return std::move(store.items);
}

} // namespace usyseek
