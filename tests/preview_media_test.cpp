#include "check.hpp"

#include <usyseek/ui/preview_math.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <print>
#include <vector>

using usyseek::ui::FitBox;
using usyseek::ui::GifCompositor;
using usyseek::ui::GifDisposal;
using usyseek::ui::SeekState;
using usyseek::ui::compute_preview_box;
using usyseek::ui::fit_scaled_size;
using usyseek::ui::is_gif_path;
using usyseek::ui::normalize_gif_delay_ms;
using usyseek::ui::normalize_mf_source_path;
using usyseek::ui::parse_netscape_loop_count;

namespace {

bool approx(float a, float b, float eps = 0.01f) noexcept {
    return std::fabs(a - b) <= eps;
}

bool approx_d(double a, double b, double eps = 1e-6) noexcept {
    return std::fabs(a - b) <= eps;
}

void test_path_and_timing_helpers() {
    CHECK(is_gif_path(L"anim.gif"));
    CHECK(is_gif_path(L"C:\\media\\CLIP.GIF"));
    CHECK(is_gif_path(L"/tmp/test.Gif"));
    CHECK(!is_gif_path(L"gif"));
    CHECK(!is_gif_path(L".gi"));
    CHECK(!is_gif_path(L"photo.png"));
    CHECK(!is_gif_path(L"clip.mp4"));

    CHECK(normalize_mf_source_path(LR"(\\?\C:\Videos\clip.mp4)") == LR"(C:\Videos\clip.mp4)");
    CHECK(normalize_mf_source_path(LR"(\\?\UNC\nas\share\clip.mp4)") == LR"(\\nas\share\clip.mp4)");
    CHECK(normalize_mf_source_path(LR"(C:\Videos\clip.mp4)") == LR"(C:\Videos\clip.mp4)");

    CHECK(normalize_gif_delay_ms(0) == 100u);
    CHECK(normalize_gif_delay_ms(1) == 100u);
    CHECK(normalize_gif_delay_ms(2) == 20u);
    CHECK(normalize_gif_delay_ms(5) == 50u);
    CHECK(normalize_gif_delay_ms(6000) == 60000u);
    CHECK(normalize_gif_delay_ms(9999) == 60000u);

    const std::array<uint8_t, 4> inf_loop{3, 1, 0, 0};
    const std::array<uint8_t, 4> finite_loop{3, 1, 0x34, 0x12};
    const std::array<uint8_t, 3> short_loop{1, 5, 0};
    const std::array<uint8_t, 4> bad_loop{2, 1, 5, 0};
    CHECK(parse_netscape_loop_count(inf_loop) == 0u);
    CHECK(parse_netscape_loop_count(finite_loop) == 0x1234u);
    CHECK(parse_netscape_loop_count(short_loop) == 5u);
    CHECK(!parse_netscape_loop_count(bad_loop).has_value());
    CHECK(!parse_netscape_loop_count({}).has_value());

    CHECK(fit_scaled_size(0, 100, 50) == (std::pair<uint32_t, uint32_t>{0u, 0u}));
    CHECK(fit_scaled_size(100, 50, 0) == (std::pair<uint32_t, uint32_t>{100u, 50u}));
    CHECK(fit_scaled_size(100, 50, 200) == (std::pair<uint32_t, uint32_t>{100u, 50u}));
    CHECK(fit_scaled_size(1920, 1080, 960) == (std::pair<uint32_t, uint32_t>{960u, 540u}));
    CHECK(fit_scaled_size(2000, 1, 100) == (std::pair<uint32_t, uint32_t>{100u, 1u}));

    // Native 1:1 display without upscaling.
    const FitBox b1 = compute_preview_box(400, 200, 0, 0, 1.0f, FitBox{10.0f, 20.0f, 800.0f, 600.0f});
    CHECK(approx(b1.left, 210.0f) && approx(b1.top, 220.0f));
    CHECK(approx(b1.width, 400.0f) && approx(b1.height, 200.0f));

    // Downscaled preview thumbnail using meta dimensions and HiDPI 2.0x scaling fits cleanly without 2x shrink/crop.
    const FitBox b2 = compute_preview_box(160, 90, 1920, 1080, 2.0f, FitBox{0.0f, 0.0f, 800.0f, 600.0f});
    CHECK(approx(b2.left, 0.0f) && approx(b2.top, 75.0f));
    CHECK(approx(b2.width, 800.0f) && approx(b2.height, 450.0f));

    // Invalid dpi_scale <= 0 safely defaults to 1.0.
    const FitBox b3 = compute_preview_box(200, 100, 0, 0, 0.0f, FitBox{0.0f, 0.0f, 400.0f, 400.0f});
    CHECK(approx(b3.width, 200.0f) && approx(b3.height, 100.0f));

    // Fractional DPI scales snap preview box edges to whole device pixels.
    for (const float scale : {1.25f, 1.5f}) {
        const FitBox bf = compute_preview_box(101, 75, 0, 0, scale, FitBox{10.0f, 20.0f, 400.0f, 300.0f});
        CHECK(approx(bf.width * scale, 101.0f));
        CHECK(approx(bf.height * scale, 75.0f));
        CHECK(approx(bf.left * scale, std::round(bf.left * scale)));
        CHECK(approx(bf.top * scale, std::round(bf.top * scale)));
    }
}

void test_gif_compositor() {
    // 2x2 canvas testing Keep, RestoreBg, RestorePrev, alpha blend, and reset.
    GifCompositor comp(2, 2);
    std::vector<uint8_t> out;

    // Frame 1: solid red at (0,0), 1x1, Keep. BGRA = [0, 0, 255, 255].
    const std::array<uint8_t, 4> red{0, 0, 255, 255};
    CHECK(comp.apply_frame(0, 0, 1, 1, red, GifDisposal::Keep, out));
    CHECK(out.size() == 16u);
    CHECK(out[2] == 255 && out[3] == 255);
    CHECK(out[4 + 3] == 0);

    // Frame 2: opaque blue at (1,0), 1x1, RestoreBg. Out must show both red & blue, next canvas clears (1,0).
    const std::array<uint8_t, 4> blue{255, 0, 0, 255};
    CHECK(comp.apply_frame(1, 0, 1, 1, blue, GifDisposal::RestoreBg, out));
    CHECK(out[2] == 255 && out[3] == 255);
    CHECK(out[4 + 0] == 255 && out[4 + 3] == 255);
    CHECK(comp.canvas()[2] == 255 && comp.canvas()[3] == 255);
    CHECK(comp.canvas()[4 + 0] == 0 && comp.canvas()[4 + 3] == 0);

    // Frame 3: opaque green over red at (0,0) with RestorePrev. Output shows green; canvas restores red.
    const std::array<uint8_t, 4> green{0, 255, 0, 255};
    CHECK(comp.apply_frame(0, 0, 1, 1, green, GifDisposal::RestorePrev, out));
    CHECK(out[1] == 255 && out[2] == 0 && out[3] == 255);
    CHECK(comp.canvas()[1] == 0 && comp.canvas()[2] == 255 && comp.canvas()[3] == 255);

    // Frame 4: semi-transparent premultiplied pixel at (0,0) blends over restored red, (1,0) stays zero.
    const std::array<uint8_t, 4> semi_blue{128, 0, 0, 128};
    CHECK(comp.apply_frame(0, 0, 1, 1, semi_blue, GifDisposal::Unspecified, out));
    // dst = [0, 0, 255, 255], inv = 127 => round(255 * 127 / 255) = 127; B = 128, R = 127, A = 255.
    CHECK(out[0] == 128 && out[1] == 0 && out[2] == 127 && out[3] == 255);

    // Clipping when sub-rect extends past edge, plus invalid undersized buffer rejection.
    const std::array<uint8_t, 16> wide{
        1, 2, 3, 255, 4, 5, 6, 255,
        7, 8, 9, 255, 10, 11, 12, 255,
    };
    CHECK(comp.apply_frame(1, 1, 2, 2, wide, GifDisposal::Keep, out));
    // Only canvas pixel (1,1) = offset 12 should receive wide[0..3].
    CHECK(out[12] == 1 && out[13] == 2 && out[14] == 3 && out[15] == 255);

    CHECK(!comp.apply_frame(0, 0, 2, 2, red, GifDisposal::Keep, out));

    comp.reset();
    for (uint8_t b : comp.canvas()) {
        CHECK(b == 0);
    }
}

void test_seek_state() {
    SeekState s;
    CHECK(!s.in_flight());
    CHECK(!s.has_pending());
    CHECK(!s.active());
    CHECK(s.started_ms() == 0);

    // Invalid inputs ignored.
    CHECK(!s.request(5.0, 0.0, 1000).has_value());
    CHECK(!s.request(std::numeric_limits<double>::quiet_NaN(), 10.0, 1000).has_value());

    // Immediate first seek, clamped to [0, duration].
    const auto r1 = s.request(-2.0, 20.0, 1000);
    CHECK(r1.has_value() && approx_d(*r1, 0.0));
    CHECK(s.in_flight());
    CHECK(!s.has_pending());
    CHECK(s.active());
    CHECK(s.started_ms() == 1000);

    // Rapid second seek within TIMEOUT_MS is queued rather than spamming Media Foundation.
    const auto r2 = s.request(12.5, 20.0, 1050);
    CHECK(!r2.has_value());
    CHECK(s.has_pending());
    CHECK(s.active());
    CHECK(s.started_ms() == 1000);
    CHECK(approx_d(s.displayed_time(1.0, false, 20.0), 12.5));

    // Newer scrub replaces pending target.
    const auto r3 = s.request(15.0, 20.0, 1100);
    CHECK(!r3.has_value());
    CHECK(approx_d(s.displayed_time(1.0, true, 20.0), 15.0));

    // Completion dispatches queued seek (15.0).
    const auto next = s.on_seeked(1120);
    CHECK(next.has_value() && approx_d(*next, 15.0));
    CHECK(s.in_flight());
    CHECK(!s.has_pending());
    CHECK(s.active());
    CHECK(s.started_ms() == 1120);

    // Second completion clears in_flight_.
    CHECK(!s.on_seeked(1140).has_value());
    CHECK(!s.in_flight());
    CHECK(!s.active());

    // While paused after seek, displayed_time continues showing 15.0 until playing settles it.
    CHECK(approx_d(s.displayed_time(14.8, false, 20.0), 15.0));
    s.on_frame_presented(true);
    CHECK(approx_d(s.displayed_time(15.2, true, 20.0), 15.2));

    // Paused settle clears show_target_ once a paused frame lands.
    CHECK(s.request(8.0, 20.0, 1500).has_value());
    CHECK(!s.on_seeked(1520).has_value());
    CHECK(approx_d(s.displayed_time(7.9, false, 20.0), 8.0));
    s.settle_paused_display();
    CHECK(approx_d(s.displayed_time(7.9, false, 20.0), 7.9));

    // Missed completion timeout override (>= 180ms) dispatches cleanly instead of getting stuck.
    CHECK(s.request(3.0, 20.0, 2000).has_value());
    CHECK(!s.request(4.0, 20.0, 2100).has_value());
    const auto timed_out = s.request(6.0, 20.0, 2000 + SeekState::TIMEOUT_MS);
    CHECK(timed_out.has_value() && approx_d(*timed_out, 6.0));
    CHECK(!s.has_pending());
    CHECK(s.started_ms() == 2000 + SeekState::TIMEOUT_MS);

    s.cancel_current();
    CHECK(!s.in_flight());
    CHECK(!s.has_pending());
    CHECK(!s.active());
    CHECK(approx_d(s.displayed_time(25.0, false, 20.0), 20.0));
}

} // namespace

int main() {
    try {
        test_path_and_timing_helpers();
        test_gif_compositor();
        test_seek_state();
    } catch (...) {
        std::println(stderr, "preview_media_test: unexpected exception");
        return 1;
    }
    std::println("preview_media_test: {} failures", usyseek::test::failures);
    return usyseek::test::failures == 0 ? 0 : 1;
}
