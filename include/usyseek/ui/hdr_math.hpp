#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace usyseek::ui::hdr {

using Vec3 = std::array<float, 3>;

/// IEC 61966-2-1 linear [0, 1] -> sRGB [0, 1] transfer function.
[[nodiscard]] inline float linear_to_srgb_channel(float x) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    if (x <= 0.0031308f) return x * 12.92f;
    return (1.055f * std::pow(x, 1.0f / 2.4f)) - 0.055f;
}

[[nodiscard]] inline Vec3 linear_to_srgb(Vec3 c) noexcept {
    return {linear_to_srgb_channel(c[0]), linear_to_srgb_channel(c[1]), linear_to_srgb_channel(c[2])};
}

/// ITU-R BT.2100 ST.2084 PQ EOTF: normalized signal [0, 1] -> linear luminance in nits [0, 10000].
[[nodiscard]] inline float pq_to_nits_channel(float v) noexcept {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    v = std::clamp(v, 0.0f, 1.0f);
    if (v <= 0.0f) return 0.0f;
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - (c3 * vp), 1e-6f);
    return std::pow(num / den, 1.0f / m1) * 10000.0f;
}

/// Converts linear ITU-R BT.2020 (D65) RGB to linear ITU-R BT.709 / sRGB (D65) RGB.
[[nodiscard]] constexpr Vec3 bt2020_to_bt709(Vec3 bt2020) noexcept {
    return {
        (1.6604910f * bt2020[0]) - (0.5876411f * bt2020[1]) - (0.0728499f * bt2020[2]),
        (-0.1245505f * bt2020[0]) + (1.1328999f * bt2020[1]) - (0.0083494f * bt2020[2]),
        (-0.0181508f * bt2020[0]) - (0.1005789f * bt2020[1]) + (1.1187297f * bt2020[2]),
    };
}

/// Normalizes linear BT.709 / scRGB (where SDR white == 1.0) into [0, 1] linear sRGB:
/// - Negative wide-gamut channels are desaturated toward Rec.709 luminance (luminance-preserving gamut clip).
/// - In-range SDR colors (peak <= 1.0) pass through bit-exact so glass matches the surrounding desktop seamlessly.
/// - HDR highlights (peak > 1.0) roll off smoothly toward white without hue shifts.
[[nodiscard]] inline Vec3 tonemap_normalized_linear(Vec3 lin) noexcept {
    for (float& ch : lin) {
        if (!std::isfinite(ch)) ch = 0.0f;
    }
    const float luma = std::max((0.2126f * lin[0]) + (0.7152f * lin[1]) + (0.0722f * lin[2]), 0.0f);
    const float mn = std::min({lin[0], lin[1], lin[2]});
    if (mn < 0.0f && luma > 1e-6f) {
        const float t = luma / (luma - mn);
        for (float& ch : lin) ch = luma + (t * (ch - luma));
    }
    for (float& ch : lin) ch = std::max(ch, 0.0f);

    const float peak = std::max({lin[0], lin[1], lin[2]});
    if (peak > 1.0f) {
        const float excess = peak - 1.0f;
        const float desat = excess / (excess + 1.5f);
        for (float& ch : lin) {
            const float base = ch / peak;
            ch = base + (desat * (1.0f - base));
        }
    }
    return lin;
}

/// Converts a linear scRGB FP16 pixel (1.0 = 80 nits, SDR white = `sdr_white_scale`) to sRGB [0, 1].
[[nodiscard]] inline Vec3 scrgb_to_srgb(Vec3 scrgb, float sdr_white_scale) noexcept {
    const float scale = std::max(sdr_white_scale, 1.0f);
    Vec3 lin{};
    for (size_t i = 0; i < 3; ++i) {
        const float v = std::isfinite(scrgb[i]) ? std::clamp(scrgb[i], -100.0f, 1000.0f) : 0.0f;
        lin[i] = v / scale;
    }
    return linear_to_srgb(tonemap_normalized_linear(lin));
}

/// Converts a normalized HDR10 ST.2084 PQ BT.2020 pixel [0, 1] to sRGB [0, 1].
[[nodiscard]] inline Vec3 pq_bt2020_to_srgb(Vec3 pq, float sdr_white_scale) noexcept {
    const float sdr_nits = std::max(sdr_white_scale, 1.0f) * 80.0f;
    const Vec3 bt2020{
        pq_to_nits_channel(pq[0]) / sdr_nits,
        pq_to_nits_channel(pq[1]) / sdr_nits,
        pq_to_nits_channel(pq[2]) / sdr_nits,
    };
    return linear_to_srgb(tonemap_normalized_linear(bt2020_to_bt709(bt2020)));
}

} // namespace usyseek::ui::hdr
