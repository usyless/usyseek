#include "check.hpp"
#include "usyseek/notification_store.hpp"

#include <cstdio>

namespace {

void run_notifications_tests() {
    using namespace usyseek;

    NotificationStoreData store;

    // 1. Insertion order is newest-first and IDs increment
    const uint64_t id1 = apply_notification(store, "warning", "Scanner", "Slow walk", "Everything unavailable", "", 100);
    const uint64_t id2 = apply_notification(store, "bogus", "Database", "Connect failed", "Timeout", "db-conn", 200);
    const uint64_t id3 = apply_notification(store, "info", "Indexer", "Notice", "Skipped 1 file", "", 300);

    CHECK(id1 == 1);
    CHECK(id2 == 2);
    CHECK(id3 == 3);
    CHECK(store.items.size() == 3);
    CHECK(store.items[0].id == id3);
    CHECK(store.items[1].id == id2);
    CHECK(store.items[1].severity == "error"); // invalid severity normalized
    CHECK(store.items[2].id == id1);

    // 2. Empty keys do not deduplicate even with identical (source, title)
    const uint64_t id4 = apply_notification(store, "warning", "Scanner", "Slow walk", "Everything unavailable", "", 400);
    CHECK(id4 == 4);
    CHECK(store.items.size() == 4);

    // 3. Matching (source, key) deduplicates, updates fields, bumps count, and promotes to index 0
    const uint64_t dup_id = apply_notification(store, "error", "Database", "Connect failed again", "Port 6334 refused", "db-conn", 500);
    CHECK(dup_id == id2);
    CHECK(store.items.size() == 4);
    CHECK(store.items[0].id == id2);
    CHECK(store.items[0].count == 2);
    CHECK(store.items[0].unix_ts == 500);
    CHECK(store.items[0].title == "Connect failed again");
    CHECK(store.items[0].detail == "Port 6334 refused");

    // 4. Same key under a different source stays distinct
    const uint64_t id5 = apply_notification(store, "warning", "UI", "Connect failed", "Timeout", "db-conn", 600);
    CHECK(id5 == 5);
    CHECK(store.items.size() == 5);

    // 5. Individual dismiss
    CHECK(!dismiss_notification(store, 9999));
    CHECK(dismiss_notification(store, id3));
    CHECK(store.items.size() == 4);
    CHECK(dismiss_notification(store, id3) == false);

    // 6. clear_notification_key scoped vs unscoped
    CHECK(!clear_notification_key(store, "", ""));
    CHECK(!clear_notification_key(store, "Indexer", "db-conn"));
    CHECK(clear_notification_key(store, "Database", "db-conn"));
    CHECK(store.items.size() == 3); // UI/db-conn remains
    CHECK(clear_notification_key(store, "", "db-conn"));
    CHECK(store.items.size() == 2);

    // 7. clear_notifications empties the list
    CHECK(clear_notifications(store));
    CHECK(store.items.empty());
    CHECK(!clear_notifications(store));

    // 8. Max capacity drops oldest entries at the tail
    NotificationStoreData capped;
    for (int i = 0; i < 10; ++i) {
        apply_notification(capped, "error", "Indexer", "Err", "Detail", "", 1000 + i, 5);
    }
    CHECK(capped.items.size() == 5);
    CHECK(capped.items[0].unix_ts == 1009);
    CHECK(capped.items[4].unix_ts == 1005);
}

} // namespace

int main() {
    try {
        run_notifications_tests();
    } catch (...) {
        (void)std::fputs("notifications_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures;
}
