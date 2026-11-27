#include <usyseek/ui/anim.hpp>
#include <usylibpp/ui/tooltip.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <print>

using usylibpp::ui::HoverTooltipBase;
using usylibpp::ui::TooltipHoverAction;
using usyseek::ui::AnimFloat;
using usyseek::ui::lerp;

namespace {

struct TestRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

void run_tooltip_tests() {
    HoverTooltipBase<TestRect> tip;
    const TestRect r_a{.left = 0.0f, .top = 0.0f, .right = 50.0f, .bottom = 24.0f};
    const TestRect r_b{.left = 60.0f, .top = 0.0f, .right = 110.0f, .bottom = 24.0f};
    const TestRect r_c{.left = 120.0f, .top = 0.0f, .right = 170.0f, .bottom = 24.0f};

    // Cold hover starts delay timer; rapid switch before tooltip ever shows still waits for timer
    assert(tip.on_hover_changed_state(&r_a, true, true, 1000) == TooltipHoverAction::StartTimer);
    assert(!tip.shown && !tip.is_warm(1100));
    assert(tip.on_hover_changed_state(&r_b, true, true, 1100) == TooltipHoverAction::StartTimer);

    // Timer fires on B -> tooltip becomes shown & warm
    assert(tip.on_timer_state(true, L"Item B", L"", true, 1480));
    assert(tip.shown && tip.title == L"Item B" && tip.anchor.left == r_b.left);
    while (tip.step(0.016f)) {}
    assert(tip.alpha.value == 1.0f);

    // Moving directly to C shows immediately without waiting or resetting opacity to 0
    assert(tip.on_hover_changed_state(&r_c, true, true, 2500) == TooltipHoverAction::ShowNow);
    assert(tip.show_now(true, L"Item C", L"Sub C", true, 2500));
    assert(tip.shown && tip.title == L"Item C" && tip.subtitle == L"Sub C" && tip.anchor.left == r_c.left);
    assert(tip.alpha.value == 1.0f);

    // Moving across an empty gap into A within WARM_WINDOW_MS still shows immediately
    assert(tip.on_hover_changed_state(nullptr, false, true, 2600) == TooltipHoverAction::None);
    assert(!tip.shown && tip.is_warm(2700));
    assert(tip.on_hover_changed_state(&r_a, true, true, 2700) == TooltipHoverAction::ShowNow);
    assert(tip.show_now(true, L"Item A", L"", true, 2700));
    assert(tip.shown && tip.title == L"Item A" && tip.anchor.left == r_a.left);

    // Moving off and waiting longer than WARM_WINDOW_MS resets to normal delay
    assert(tip.on_hover_changed_state(nullptr, false, true, 3000) == TooltipHoverAction::None);
    assert(tip.on_hover_changed_state(&r_b, true, true, 3000 + HoverTooltipBase<TestRect>::WARM_WINDOW_MS + 1) ==
           TooltipHoverAction::StartTimer);

    // Explicit clear_state (click/key/leave) immediately resets warm state
    assert(tip.on_timer_state(true, L"Item B", L"", true, 4000));
    assert(tip.shown);
    tip.clear_state(false);
    assert(!tip.shown && !tip.is_warm(4010));
    assert(tip.on_hover_changed_state(&r_c, true, true, 4010) == TooltipHoverAction::StartTimer);
}

void run_anim_tests() {
    assert(std::abs(lerp(10.0f, 30.0f, 0.25f) - 15.0f) < 1e-6f);

    AnimFloat a;
    assert(a.value == 0.0f && a.target == 0.0f);
    assert(!a.active());
    assert(!a.step(0.016f));

    // Snap / non-animated set completes immediately
    a.set(2.0f, false);
    assert(a.value == 2.0f && a.target == 2.0f);
    assert(!a.active());
    assert(!a.step(0.016f));

    // Animated set moves monotonically toward target and stops returning true once settled
    a.snap(0.0f);
    a.set(1.0f, true);
    assert(a.active());
    int steps = 0;
    float prev = a.value;
    while (a.step(0.016f)) {
        assert(a.value > prev && a.value <= 1.0f);
        prev = a.value;
        ++steps;
        assert(steps < 200);
    }
    assert(steps > 1);
    assert(a.value == 1.0f);
    assert(!a.active());
    assert(!a.step(0.016f));

    std::println("anim_test: all assertions passed ({} frames to converge)", steps);
}

} // namespace

int main() {
    try {
        run_anim_tests();
        run_tooltip_tests();
    } catch (...) {
        (void)std::fputs("anim_test: unexpected exception\n", stderr);
        return 1;
    }
    return 0;
}
