#include "check.hpp"

#include <usyseek/ui/pagination.hpp>

#include <cstdint>
#include <print>
#include <vector>

using usyseek::ui::PaginationTracker;

namespace {

void run_pagination_tests() {
    PaginationTracker tr;
    std::vector<uint64_t> items;
    auto push_page = [&](std::vector<uint64_t> page, bool append, bool ok, uint64_t next_off) {
        return tr.apply_page(
            page, append, ok, next_off,
            [](uint64_t id) { return id; },
            [&](uint64_t id) { items.push_back(id); });
    };

    // 1. Initial page keeps order, deduplicates intra-page repeats, and uses raw next_offset
    // (e.g. similar: erased self_id leaving 3 points out of 4 fetched).
    CHECK(push_page({10, 20, 10, 30}, false, true, 4));
    CHECK(items == (std::vector<uint64_t>{10, 20, 30}));
    CHECK(tr.unique_count() == 3);
    CHECK(tr.next_offset() == 4);

    // 2. Appended page drops IDs already seen on page 1 so distinct cards never share pt.id.
    CHECK(push_page({20, 40, 30, 50}, true, true, 8));
    CHECK(items == (std::vector<uint64_t>{10, 20, 30, 40, 50}));
    CHECK(tr.unique_count() == 5);
    CHECK(tr.next_offset() == 8);

    // 3. Append error clears has_more (returns false) while preserving existing items and offset.
    CHECK(!push_page({60}, true, false, 0));
    CHECK(items == (std::vector<uint64_t>{10, 20, 30, 40, 50}));
    CHECK(tr.next_offset() == 8);

    // 4. Stalled append (no offset advance) returns false to stop infinite pagination loops.
    CHECK(!push_page({50}, true, true, 8));
    CHECK(items == (std::vector<uint64_t>{10, 20, 30, 40, 50}));

    // 5. Fresh search resets seen IDs and offset so earlier IDs can appear again.
    items.clear();
    CHECK(push_page({20, 60}, false, true, 2));
    CHECK(items == (std::vector<uint64_t>{20, 60}));
    CHECK(tr.unique_count() == 2);
    CHECK(tr.next_offset() == 2);

    // 6. Failed fresh search resets tracker state.
    items.clear();
    CHECK(!push_page({99}, false, false, 1));
    CHECK(items.empty());
    CHECK(tr.unique_count() == 0);
    CHECK(tr.next_offset() == 0);
}

} // namespace

int main() {
    try {
        run_pagination_tests();
    } catch (...) {
        std::println(stderr, "pagination_test: unexpected exception");
        return 1;
    }
    std::println("pagination_test: {} failures", usyseek::test::failures);
    return usyseek::test::failures == 0 ? 0 : 1;
}
