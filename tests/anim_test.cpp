#include <usyseek/ui/anim.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <print>

using usyseek::ui::AnimFloat;
using usyseek::ui::lerp;

namespace {

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
    } catch (...) {
        (void)std::fputs("anim_test: unexpected exception\n", stderr);
        return 1;
    }
    return 0;
}
