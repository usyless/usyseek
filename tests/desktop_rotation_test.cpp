#include "check.hpp"

#include <usyseek/ui/desktop_rotation.hpp>

#include <array>

using namespace usyseek::ui;

namespace {

struct Pt {
    float x, y;
};

Pt apply(const Affine2D& m, float x, float y) {
    return {.x = (m.m11 * x) + (m.m21 * y) + m.dx, .y = (m.m12 * x) + (m.m22 * y) + m.dy};
}

} // namespace

int main() {
    // A 1080x1920 portrait monitor whose panel is natively 1920x1080.
    constexpr long dw = 1080;
    constexpr long dh = 1920;
    constexpr IntRect tex_rc{.left = 100, .top = 200, .right = 300, .bottom = 250};

    for (const DesktopRotation rot : {DesktopRotation::Identity, DesktopRotation::Rot90, DesktopRotation::Rot180, DesktopRotation::Rot270}) {
        const bool swap = rot == DesktopRotation::Rot90 || rot == DesktopRotation::Rot270;
        const long w = swap ? dw : dh;
        const long h = swap ? dh : dw;
        // The two rect maps are inverses.
        const IntRect desk = tex_to_desktop(tex_rc, rot, w, h);
        CHECK((desktop_to_tex(desk, rot, w, h) == tex_rc));
        // The full texture lands exactly on the full desktop.
        const IntRect full_tex{.left = 0, .top = 0, .right = swap ? h : w, .bottom = swap ? w : h};
        CHECK((tex_to_desktop(full_tex, rot, w, h) == IntRect{.left = 0, .top = 0, .right = w, .bottom = h}));
        // The transform agrees with the rect map: the texel at (u, v) covers the desktop pixel the rect map says.
        const Affine2D m = tex_to_desktop_xform(rot, w, h, 0.0f, 0.0f);
        const Pt c = apply(m, 100.5f, 200.5f);
        const IntRect px = tex_to_desktop(IntRect{.left = 100, .top = 200, .right = 101, .bottom = 201}, rot, w, h);
        CHECK(c.x > static_cast<float>(px.left));
        CHECK(c.x < static_cast<float>(px.right));
        CHECK(c.y > static_cast<float>(px.top));
        CHECK(c.y < static_cast<float>(px.bottom));
    }

    // Rot90: the texture's top-left ends up at the desktop's top-right.
    CHECK((tex_to_desktop(IntRect{.left = 0, .top = 0, .right = 1, .bottom = 1}, DesktopRotation::Rot90, dw, dh) ==
           IntRect{.left = dw - 1, .top = 0, .right = dw, .bottom = 1}));
    // Rot270: the texture's top-left ends up at the desktop's bottom-left.
    CHECK((tex_to_desktop(IntRect{.left = 0, .top = 0, .right = 1, .bottom = 1}, DesktopRotation::Rot270, dw, dh) ==
           IntRect{.left = 0, .top = dh - 1, .right = 1, .bottom = dh}));

    // Offsets shift into window space.
    const Pt o = apply(tex_to_desktop_xform(DesktopRotation::Identity, dw, dh, -50.0f, 10.0f), 60.0f, 0.0f);
    CHECK(o.x == 10.0f);
    CHECK(o.y == 10.0f);

    CHECK((int_rect_intersect(IntRect{0, 0, 10, 10}, IntRect{5, 5, 20, 20}) == IntRect{5, 5, 10, 10}));
    CHECK(int_rect_empty(int_rect_intersect(IntRect{0, 0, 10, 10}, IntRect{10, 0, 20, 10})));

    // Subtraction: a hole in the middle leaves four bands covering exactly the rest.
    std::array<IntRect, 4> parts{};
    const size_t n = int_rect_subtract(IntRect{.left = 0, .top = 0, .right = 100, .bottom = 100}, IntRect{.left = 40, .top = 40, .right = 60, .bottom = 60}, parts);
    CHECK(n == 4);
    long area = 0;
    for (size_t i = 0; i < n; ++i) {
        CHECK(int_rect_empty(int_rect_intersect(parts[i], IntRect{40, 40, 60, 60})));
        area += (parts[i].right - parts[i].left) * (parts[i].bottom - parts[i].top);
    }
    CHECK(area == (100 * 100) - (20 * 20));
    CHECK(int_rect_subtract(IntRect{0, 0, 10, 10}, IntRect{20, 20, 30, 30}, parts) == 1);
    CHECK((parts[0] == IntRect{0, 0, 10, 10}));
    CHECK(int_rect_subtract(IntRect{0, 0, 10, 10}, IntRect{-5, -5, 15, 15}, parts) == 0);
    CHECK(int_rect_subtract(IntRect{0, 0, 10, 10}, IntRect{5, -5, 15, 15}, parts) == 1);
    CHECK((parts[0] == IntRect{0, 0, 5, 10}));

    return usyseek::test::failures != 0;
}
