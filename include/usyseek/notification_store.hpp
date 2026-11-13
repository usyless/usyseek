#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace usyseek {

inline constexpr size_t MAX_NOTIFICATIONS = 200;

struct Notification {
    uint64_t id = 0;
    int64_t unix_ts = 0;
    std::string severity = "error"; // "error" | "warning" | "info"
    std::string source;
    std::string title;
    std::string detail;
    std::string key;
    uint32_t count = 1;
};

struct NotificationStoreData {
    uint64_t next_id = 1;
    std::vector<Notification> items;
};

inline bool is_valid_severity(std::string_view s) noexcept {
    return s == "error" || s == "warning" || s == "info";
}

/// Inserts a notification at index 0 (newest first), deduplicating by non-empty `(source, key)`.
inline uint64_t apply_notification(NotificationStoreData& store, std::string_view severity, std::string_view source,
                                   std::string_view title, std::string_view detail, std::string_view key,
                                   int64_t unix_ts, size_t max_items = MAX_NOTIFICATIONS) {
    const std::string sev = is_valid_severity(severity) ? std::string{severity} : std::string{"error"};
    if (!key.empty()) {
        const auto it = std::ranges::find_if(store.items, [&](const Notification& n) {
            return n.key == key && n.source == source;
        });
        if (it != store.items.end()) {
            it->severity = sev;
            it->title = std::string{title};
            it->detail = std::string{detail};
            it->unix_ts = unix_ts;
            if (it->count < UINT32_MAX) ++it->count;
            const uint64_t id = it->id;
            std::rotate(store.items.begin(), it, it + 1);
            return id;
        }
    }

    if (store.next_id == 0) store.next_id = 1;
    const uint64_t id = store.next_id++;
    Notification n;
    n.id = id;
    n.unix_ts = unix_ts;
    n.severity = sev;
    n.source = std::string{source};
    n.title = std::string{title};
    n.detail = std::string{detail};
    n.key = std::string{key};
    n.count = 1;
    store.items.insert(store.items.begin(), std::move(n));
    if (max_items > 0 && store.items.size() > max_items) {
        store.items.resize(max_items);
    }
    return id;
}

/// Removes the notification matching `id`. Returns true if an item was removed.
inline bool dismiss_notification(NotificationStoreData& store, uint64_t id) {
    const auto it = std::ranges::find_if(store.items, [id](const Notification& n) { return n.id == id; });
    if (it == store.items.end()) return false;
    store.items.erase(it);
    return true;
}

/// Removes all notifications matching `key` (or `(source, key)` when `source` is non-empty).
inline bool clear_notification_key(NotificationStoreData& store, std::string_view source, std::string_view key) {
    if (key.empty()) return false;
    const size_t n = std::erase_if(store.items, [&](const Notification& item) {
        return item.key == key && (source.empty() || item.source == source);
    });
    return n > 0;
}

/// Removes all notifications. Returns true when any items were cleared.
inline bool clear_notifications(NotificationStoreData& store) noexcept {
    if (store.items.empty()) return false;
    store.items.clear();
    return true;
}

} // namespace usyseek
