#include "check.hpp"

#include <usyseek/ui/window_lifecycle.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <print>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif

using usyseek::ui::CARET_BLINK_INFINITE;
using usyseek::ui::CaretDecision;
using usyseek::ui::DEFAULT_CARET_BLINK_MS;
using usyseek::ui::caret_blink_interval_ms;
using usyseek::ui::caret_timer_decision;
using usyseek::ui::should_allow_close;
using usyseek::ui::should_keep_preview_decode;
using usyseek::ui::window_can_render_live;
using usyseek::ui::wm_activate_is_active;

static_assert(caret_blink_interval_ms(530u) == 530u);
static_assert(caret_blink_interval_ms(0u) == DEFAULT_CARET_BLINK_MS);
static_assert(!caret_blink_interval_ms(CARET_BLINK_INFINITE).has_value());
static_assert(!wm_activate_is_active(0u, false));
static_assert(wm_activate_is_active(1u, false));
static_assert(wm_activate_is_active(2u, false));
static_assert(!wm_activate_is_active(1u, true));
static_assert(!wm_activate_is_active(2u, true));

namespace {

void test_caret_blink_interval() {
    CHECK(caret_blink_interval_ms(100u) == 100u);
    CHECK(caret_blink_interval_ms(530u) == 530u);
    CHECK(caret_blink_interval_ms(1200u) == 1200u);
    CHECK(caret_blink_interval_ms(0u) == DEFAULT_CARET_BLINK_MS);
    CHECK(!caret_blink_interval_ms(CARET_BLINK_INFINITE).has_value());
}

void test_wm_activate_decoding() {
    // WA_INACTIVE == 0, WA_ACTIVE == 1, WA_CLICKACTIVE == 2
    CHECK(!wm_activate_is_active(0u, false));
    CHECK(!wm_activate_is_active(0u, true));
    CHECK(wm_activate_is_active(1u, false));
    CHECK(wm_activate_is_active(2u, false));
    // Minimized window must not be treated as active even if LOWORD(wp) != WA_INACTIVE.
    CHECK(!wm_activate_is_active(1u, true));
    CHECK(!wm_activate_is_active(2u, true));
}

void test_caret_timer_decision_exhaustive() {
    const std::array<std::optional<uint32_t>, 3> blinks{std::nullopt, 530u, 250u};
    for (const bool visible : {false, true}) {
        for (const bool minimized : {false, true}) {
            for (const bool fading : {false, true}) {
                for (const bool active : {false, true}) {
                    for (const bool focused : {false, true}) {
                        for (const auto blink : blinks) {
                            const CaretDecision d = caret_timer_decision(visible, minimized, fading, active, focused, blink);
                            const bool should_show = visible && !minimized && !fading && active && focused;
                            CHECK(d.visible == should_show);
                            CHECK(d.run_timer == (should_show && blink.has_value()));
                        }
                    }
                }
            }
        }
    }
}

void test_live_render_preview_and_close_guards() {
    CHECK(window_can_render_live(true, false, false));
    CHECK(!window_can_render_live(false, false, false));
    CHECK(!window_can_render_live(true, true, false));
    CHECK(!window_can_render_live(true, false, true));
    CHECK(!window_can_render_live(false, true, true));

    CHECK(should_keep_preview_decode(true, false, 0));
    CHECK(should_keep_preview_decode(true, false, 42));
    CHECK(!should_keep_preview_decode(true, false, -1));
    CHECK(!should_keep_preview_decode(false, false, 0));
    CHECK(!should_keep_preview_decode(true, true, 0));

    CHECK(should_allow_close(false, false));
    CHECK(!should_allow_close(true, false));
    CHECK(!should_allow_close(false, true));
    CHECK(!should_allow_close(true, true));
}

#ifdef _WIN32
struct DummyWindow {
    HWND hwnd = nullptr;
    bool active = false;
    bool nc_destroyed = false;
    int timer_ticks = 0;

    static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<DummyWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (msg == WM_NCDESTROY) {
            if (auto* self = reinterpret_cast<DummyWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA))) {
                self->nc_destroyed = true;
                self->hwnd = nullptr;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            }
            return DefWindowProcW(h, msg, wp, lp);
        }
        if (auto* self = reinterpret_cast<DummyWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA))) {
            switch (msg) {
                case WM_ACTIVATE:
                    self->active = wm_activate_is_active(LOWORD(wp), HIWORD(wp) != 0);
                    break; // must fall through to DefWindowProcW
                case WM_TIMER:
                    ++self->timer_ticks;
                    return 0;
                case WM_DESTROY:
                    KillTimer(h, 1);
                    return 0;
                default:
                    break;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

void test_win32_window_lifecycle_contracts() {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    constexpr const wchar_t* kClass = L"UsyseekLifecycleTestWindow";
    WNDCLASSEXW wc{.cbSize = sizeof(wc)};
    wc.lpfnWndProc = &DummyWindow::wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    DummyWindow w1;
    DummyWindow w2;
    DummyWindow w_min;
    HWND h1 = CreateWindowExW(0, kClass, L"T1", WS_POPUP, -32000, -32000, 64, 64, nullptr, nullptr, inst, &w1);
    HWND h2 = CreateWindowExW(0, kClass, L"T2", WS_POPUP, -32000, -32000, 64, 64, nullptr, nullptr, inst, &w2);
    HWND h_min = CreateWindowExW(0, kClass, L"TMin", WS_OVERLAPPEDWINDOW | WS_MINIMIZE, -32000, -32000, 64, 64, nullptr, nullptr, inst, &w_min);
    CHECK(h1 != nullptr && w1.hwnd == h1);
    CHECK(h2 != nullptr && w2.hwnd == h2);
    CHECK(h_min != nullptr && w_min.hwnd == h_min && IsIconic(h_min) != FALSE);

    // Owned capture check: releasing capture only when GetCapture() == self never steals a sibling window's capture.
    SetCapture(h2);
    CHECK(GetCapture() == h2);
    if (GetCapture() == h1) ReleaseCapture();
    CHECK(GetCapture() == h2);
    if (GetCapture() == h2) ReleaseCapture();
    CHECK(GetCapture() == nullptr);

    // WM_ACTIVATE decoding for minimized vs normal activation (with fall-through to DefWindowProcW).
    SendMessageW(h_min, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, TRUE), 0);
    CHECK(!w_min.active);
    SendMessageW(h1, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, FALSE), 0);
    CHECK(w1.active);
    SendMessageW(h1, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, FALSE), 0);
    CHECK(!w1.active);

    // Timer started on h1 is killed on DestroyWindow and GWLP_USERDATA is cleared in WM_NCDESTROY.
    CHECK(SetTimer(h1, 1, 10, nullptr) != 0);
    DestroyWindow(h1);
    DestroyWindow(h2);
    DestroyWindow(h_min);
    CHECK(w1.nc_destroyed && w1.hwnd == nullptr);
    CHECK(w2.nc_destroyed && w2.hwnd == nullptr);
    CHECK(w_min.nc_destroyed && w_min.hwnd == nullptr);
    CHECK(!IsWindow(h1) && !IsWindow(h2) && !IsWindow(h_min));

    UnregisterClassW(kClass, inst);
}
#endif

} // namespace

int main() {
    test_caret_blink_interval();
    test_wm_activate_decoding();
    test_caret_timer_decision_exhaustive();
    test_live_render_preview_and_close_guards();
#ifdef _WIN32
    test_win32_window_lifecycle_contracts();
#endif
    if (usyseek::test::failures > 0) {
        std::println(stderr, "{} failure(s)", usyseek::test::failures);
        return 1;
    }
    std::println("window_lifecycle_test: ok");
    return 0;
}
