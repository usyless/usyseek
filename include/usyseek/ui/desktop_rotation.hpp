#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace usyseek::ui {

/// Orientation of a duplicated output. DXGI Output Duplication hands back the frame in the panel's native
/// (unrotated) orientation, so rotated monitors need their texture mapped into desktop space.
enum class DesktopRotation : uint8_t { Identity, Rot90, Rot180, Rot270 };

/// Integer rect, edges exclusive on right/bottom (same convention as Win32 RECT).
struct IntRect {
    long left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const IntRect&) const = default;
};

/// 2x3 affine matrix with the D2D1_MATRIX_3X2_F layout: x' = m11*x + m21*y + dx, y' = m12*x + m22*y + dy.
struct Affine2D {
    float m11 = 1.0f, m12 = 0.0f, m21 = 0.0f, m22 = 1.0f, dx = 0.0f, dy = 0.0f;
    bool operator==(const Affine2D&) const = default;
};

/// Texture-space rect -> monitor-local desktop rect. `dw`/`dh` are the monitor's desktop (rotated) extents.
[[nodiscard]] constexpr IntRect tex_to_desktop(IntRect r, DesktopRotation rot, long dw, long dh) noexcept {
    switch (rot) {
        case DesktopRotation::Rot90: return {.left = dw - r.bottom, .top = r.left, .right = dw - r.top, .bottom = r.right};
        case DesktopRotation::Rot180: return {.left = dw - r.right, .top = dh - r.bottom, .right = dw - r.left, .bottom = dh - r.top};
        case DesktopRotation::Rot270: return {.left = r.top, .top = dh - r.right, .right = r.bottom, .bottom = dh - r.left};
        case DesktopRotation::Identity: break;
    }
    return r;
}

/// Monitor-local desktop rect -> texture-space rect (inverse of `tex_to_desktop`).
[[nodiscard]] constexpr IntRect desktop_to_tex(IntRect r, DesktopRotation rot, long dw, long dh) noexcept {
    switch (rot) {
        case DesktopRotation::Rot90: return {.left = r.top, .top = dw - r.right, .right = r.bottom, .bottom = dw - r.left};
        case DesktopRotation::Rot180: return {.left = dw - r.right, .top = dh - r.bottom, .right = dw - r.left, .bottom = dh - r.top};
        case DesktopRotation::Rot270: return {.left = dh - r.bottom, .top = r.left, .right = dh - r.top, .bottom = r.right};
        case DesktopRotation::Identity: break;
    }
    return r;
}

/// Transform that draws texture pixels at their monitor-local desktop position, then shifts by (`ox`, `oy`).
[[nodiscard]] constexpr Affine2D tex_to_desktop_xform(DesktopRotation rot, long dw, long dh, float ox, float oy) noexcept {
    const auto w = static_cast<float>(dw);
    const auto h = static_cast<float>(dh);
    switch (rot) {
        case DesktopRotation::Rot90: return {.m11 = 0.0f, .m12 = 1.0f, .m21 = -1.0f, .m22 = 0.0f, .dx = w + ox, .dy = oy};
        case DesktopRotation::Rot180: return {.m11 = -1.0f, .m12 = 0.0f, .m21 = 0.0f, .m22 = -1.0f, .dx = w + ox, .dy = h + oy};
        case DesktopRotation::Rot270: return {.m11 = 0.0f, .m12 = -1.0f, .m21 = 1.0f, .m22 = 0.0f, .dx = ox, .dy = h + oy};
        case DesktopRotation::Identity: break;
    }
    return {.dx = ox, .dy = oy};
}

[[nodiscard]] constexpr IntRect int_rect_intersect(IntRect a, IntRect b) noexcept {
    const IntRect r{
        .left = a.left > b.left ? a.left : b.left,
        .top = a.top > b.top ? a.top : b.top,
        .right = a.right < b.right ? a.right : b.right,
        .bottom = a.bottom < b.bottom ? a.bottom : b.bottom,
    };
    return (r.right > r.left && r.bottom > r.top) ? r : IntRect{};
}

[[nodiscard]] constexpr bool int_rect_empty(IntRect r) noexcept {
    return r.right <= r.left || r.bottom <= r.top;
}

/// Writes `a` minus `b` into `out` as up to four non-overlapping rects (top band, bottom band, left, right) and
/// returns how many were written.
constexpr size_t int_rect_subtract(IntRect a, IntRect b, std::array<IntRect, 4>& out) noexcept {
    if (int_rect_empty(a)) return 0;
    const IntRect cut = int_rect_intersect(a, b);
    if (int_rect_empty(cut)) {
        out[0] = a;
        return 1;
    }
    size_t n = 0;
    if (cut.top > a.top) out[n++] = {.left = a.left, .top = a.top, .right = a.right, .bottom = cut.top};
    if (cut.bottom < a.bottom) out[n++] = {.left = a.left, .top = cut.bottom, .right = a.right, .bottom = a.bottom};
    if (cut.left > a.left) out[n++] = {.left = a.left, .top = cut.top, .right = cut.left, .bottom = cut.bottom};
    if (cut.right < a.right) out[n++] = {.left = cut.right, .top = cut.top, .right = a.right, .bottom = cut.bottom};
    return n;
}

} // namespace usyseek::ui
