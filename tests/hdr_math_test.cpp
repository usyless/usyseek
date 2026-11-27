#include "check.hpp"

#include <usyseek/ui/hdr_math.hpp>

#include <cmath>
#include <limits>

using namespace usyseek::ui::hdr;

namespace {

bool approx(float a, float b, float eps = 1e-3f) noexcept {
    return std::fabs(a - b) <= eps;
}

} // namespace

int main() {
    // 1. sRGB transfer function endpoints & mid-gray.
    CHECK(approx(linear_to_srgb_channel(0.0f), 0.0f, 1e-6f));
    CHECK(approx(linear_to_srgb_channel(1.0f), 1.0f, 1e-6f));
    CHECK(approx(linear_to_srgb_channel(0.214041f), 0.5f, 2e-3f));

    // 2. scRGB SDR content at arbitrary Windows SDR white level (e.g. 240 nits => scale 3.0)
    //    maps bit-accurately back to sRGB [0, 1].
    {
        constexpr float white_scale = 3.0f; // 240 nits
        const Vec3 black = scrgb_to_srgb({0.0f, 0.0f, 0.0f}, white_scale);
        CHECK(approx(black[0], 0.0f, 1e-6f));
        CHECK(approx(black[1], 0.0f, 1e-6f));
        CHECK(approx(black[2], 0.0f, 1e-6f));

        const Vec3 white = scrgb_to_srgb({white_scale, white_scale, white_scale}, white_scale);
        CHECK(approx(white[0], 1.0f, 1e-5f));
        CHECK(approx(white[1], 1.0f, 1e-5f));
        CHECK(approx(white[2], 1.0f, 1e-5f));

        // Linear 0.5 * white_scale -> sRGB ~0.7354
        const Vec3 mid = scrgb_to_srgb({0.5f * white_scale, 0.25f * white_scale, 0.0f}, white_scale);
        CHECK(approx(mid[0], linear_to_srgb_channel(0.5f), 1e-5f));
        CHECK(approx(mid[1], linear_to_srgb_channel(0.25f), 1e-5f));
        CHECK(approx(mid[2], 0.0f, 1e-6f));
    }

    // 3. Wide-gamut negative scRGB channel is clipped while preserving Rec.709 luminance.
    {
        const Vec3 wide{0.8f, -0.1f, 0.2f};
        const float orig_luma = (0.2126f * wide[0]) + (0.7152f * wide[1]) + (0.0722f * wide[2]);
        const Vec3 clipped = tonemap_normalized_linear(wide);
        CHECK(clipped[0] >= 0.0f && clipped[1] >= 0.0f && clipped[2] >= 0.0f);
        const float new_luma = (0.2126f * clipped[0]) + (0.7152f * clipped[1]) + (0.0722f * clipped[2]);
        CHECK(approx(orig_luma, new_luma, 1e-4f));
    }

    // 4. HDR highlights (> 1.0) are continuous at peak == 1.0 and smoothly desaturate toward white.
    {
        const Vec3 at_one = tonemap_normalized_linear({1.0f, 0.4f, 0.2f});
        const Vec3 just_above = tonemap_normalized_linear({1.0001f, 0.4f, 0.2f});
        CHECK(approx(at_one[0], just_above[0], 1e-3f));
        CHECK(approx(at_one[1], just_above[1], 1e-3f));
        CHECK(approx(at_one[2], just_above[2], 1e-3f));

        const Vec3 bright_red = tonemap_normalized_linear({6.0f, 0.6f, 0.3f});
        CHECK(approx(bright_red[0], 1.0f, 1e-5f));
        CHECK(bright_red[1] > 0.6f && bright_red[1] < 1.0f);
        CHECK(bright_red[2] > 0.3f && bright_red[2] < bright_red[1]);
    }

    // 5. NaN / Inf safety.
    {
        const float qnan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const Vec3 bad = scrgb_to_srgb({qnan, inf, -inf}, 2.0f);
        for (const float c : bad) {
            CHECK(std::isfinite(c));
            CHECK(c >= 0.0f && c <= 1.0f);
        }
    }

    // 6. BT.2020 -> BT.709 preserves D65 white point.
    {
        const Vec3 w = bt2020_to_bt709({1.0f, 1.0f, 1.0f});
        CHECK(approx(w[0], 1.0f, 1e-5f));
        CHECK(approx(w[1], 1.0f, 1e-5f));
        CHECK(approx(w[2], 1.0f, 1e-5f));
    }

    // 7. ST.2084 PQ EOTF checks: 0 -> 0 nits, 1 -> 10000 nits, ~0.5807 -> ~203 nits.
    {
        CHECK(approx(pq_to_nits_channel(0.0f), 0.0f, 1e-5f));
        CHECK(approx(pq_to_nits_channel(1.0f), 10000.0f, 1.0f));
        CHECK(approx(pq_to_nits_channel(0.58068888f), 203.0f, 1.0f));

        // 240-nit SDR white in PQ (~0.5973) with white_scale 3.0 -> sRGB white (1, 1, 1).
        const Vec3 pq_white = pq_bt2020_to_srgb({0.5973f, 0.5973f, 0.5973f}, 3.0f);
        CHECK(approx(pq_white[0], 1.0f, 1e-2f));
        CHECK(approx(pq_white[1], 1.0f, 1e-2f));
        CHECK(approx(pq_white[2], 1.0f, 1e-2f));
    }

    return usyseek::test::failures != 0;
}
