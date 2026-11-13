#include "check.hpp"

#include <usyseek/ui/dirty_rects.hpp>

#include <array>
#include <print>

using usyseek::ui::FlipDirtyTracker;
using usyseek::ui::PixelRect;
using usyseek::ui::dip_to_pixel_rect;
using usyseek::ui::merge_disjoint_rects;

namespace {

void run_dirty_rects_tests() {
    // 1. DIP -> pixel conversion with padding, fractional DPI scaling, and clamping
    CHECK((dip_to_pixel_rect(10.0f, 20.0f, 10.0f, 30.0f, 1.5f, 1000, 1000).empty()));
    CHECK((dip_to_pixel_rect(-50.0f, -50.0f, -10.0f, -10.0f, 1.0f, 100, 100).empty()));
    const PixelRect p = dip_to_pixel_rect(10.2f, 20.4f, 30.6f, 40.8f, 1.25f, 200, 200, 2.0f);
    // floor((10.2 - 2)*1.25 = 10.25) -> 10; ceil((30.6 + 2)*1.25 = 40.75) -> 41
    CHECK(p == (PixelRect{.left = 10, .top = 23, .right = 41, .bottom = 54}));
    const PixelRect clamped = dip_to_pixel_rect(-1.0f, -1.0f, 150.0f, 150.0f, 1.0f, 100, 100, 2.0f);
    CHECK(clamped == (PixelRect{.left = 0, .top = 0, .right = 100, .bottom = 100}));

    // 2. Transitive cascading merge into strictly pairwise disjoint rectangles
    const std::array<PixelRect, 3> a = {{
        {.left = 10, .top = 10, .right = 30, .bottom = 30},
        {.left = 50, .top = 50, .right = 70, .bottom = 70},
        {.left = 0, .top = 0, .right = 0, .bottom = 0},
    }};
    const std::array<PixelRect, 1> b = {{{.left = 25, .top = 25, .right = 55, .bottom = 55}}};
    const auto merged = merge_disjoint_rects(a, b);
    CHECK(merged.size() == 1);
    CHECK(merged[0] == (PixelRect{.left = 10, .top = 10, .right = 70, .bottom = 70}));

    // Touching (adjacent, zero-overlap) rectangles stay separate
    const std::array<PixelRect, 2> adj = {{
        {.left = 0, .top = 0, .right = 10, .bottom = 10},
        {.left = 10, .top = 0, .right = 20, .bottom = 10},
    }};
    const auto adj_merged = merge_disjoint_rects(adj);
    CHECK(adj_merged.size() == 2);

    // 3. FlipDirtyTracker 2-buffer invariant
    FlipDirtyTracker tr;
    CHECK(tr.full_sync_remaining() == 2);

    // First frame after create/resize must be full, even if partial requested
    const std::array<PixelRect, 1> d1 = {{{.left = 10, .top = 10, .right = 30, .bottom = 30}}};
    const auto f1 = tr.plan(false, d1, 1000, 1000);
    CHECK(!f1.skip);
    CHECK(f1.full);
    tr.commit(false, f1);
    CHECK(tr.full_sync_remaining() == 1);
    CHECK(tr.prev_dirty().empty());

    // Second frame promotes partial to full while recording curr_dirty for the other buffer
    const auto f2 = tr.plan(false, d1, 1000, 1000);
    CHECK(!f2.skip);
    CHECK(f2.full);
    tr.commit(false, f2);
    CHECK(tr.full_sync_remaining() == 0);
    CHECK(tr.prev_dirty().size() == 1);

    // Third frame updates previous + current dirty rect only
    const std::array<PixelRect, 1> d3 = {{{.left = 100, .top = 100, .right = 120, .bottom = 120}}};
    const auto f3 = tr.plan(false, d3, 1000, 1000);
    CHECK(!f3.skip);
    CHECK(!f3.full);
    CHECK(f3.render_rects.size() == 2);
    tr.commit(false, f3);
    CHECK(tr.prev_dirty().size() == 1);
    CHECK(tr.prev_dirty()[0] == d3[0]);

    // Frame with no current change still syncs previous dirty rect into back buffer
    const auto f4 = tr.plan(false, {}, 1000, 1000);
    CHECK(!f4.skip);
    CHECK(!f4.full);
    CHECK(f4.render_rects.size() == 1);
    CHECK(f4.render_rects[0] == d3[0]);
    tr.commit(false, f4);
    CHECK(tr.prev_dirty().empty());

    // Next idle frame skips completely
    const auto f5 = tr.plan(false, {}, 1000, 1000);
    CHECK(f5.skip);

    // Huge dirty rect collapses to full (>70% area) without losing buffer sync
    const std::array<PixelRect, 1> huge = {{{.left = 0, .top = 0, .right = 900, .bottom = 850}}};
    const auto f6 = tr.plan(false, huge, 1000, 1000);
    CHECK(!f6.skip);
    CHECK(f6.full);
    tr.commit(false, f6);
    CHECK(tr.full_sync_remaining() == 0);
    CHECK(tr.prev_dirty().size() == 1);

    // Explicit full redraw leaves 1 remaining full sync for the other buffer
    const auto f7 = tr.plan(true, {}, 1000, 1000);
    CHECK(f7.full);
    tr.commit(true, f7);
    CHECK(tr.full_sync_remaining() == 1);
    CHECK(tr.prev_dirty().empty());
}

} // namespace

int main() {
    try {
        run_dirty_rects_tests();
    } catch (...) {
        std::println(stderr, "dirty_rects_test: unexpected exception");
        return 1;
    }
    std::println("dirty_rects_test: {} failures", usyseek::test::failures);
    return usyseek::test::failures == 0 ? 0 : 1;
}
