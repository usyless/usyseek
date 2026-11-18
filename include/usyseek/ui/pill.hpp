#pragma once

#include "gif_player.hpp"
#include "pagination.hpp"
#include "shared.hpp"
#include "textfield.hpp"
#include "video_player.hpp"

#include <shellscalingapi.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <ranges>
#include <set>
#include <unordered_set>
#include <utility>

namespace usyseek::ui {

class Pill {
public:
    explicit Pill(Shared& s) : sh_{s} {}

    bool create(HINSTANCE inst) {
        WNDCLASSEXW wc{.cbSize = sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Pill::wndproc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = PILL_CLASS;
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED, PILL_CLASS, APP_NAME,
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        applied_pad_ = active_pad();
        apply_window_theme(hwnd_, sh_.theme, true, true);
        if (!target_.attach(sh_.gfx, hwnd_, glass_active())) return false;
        gfx_epoch_ = sh_.gfx.epoch();
        gif_.set_notify(hwnd_, WM_APP_GIF);
        video_.set_notify(hwnd_, WM_APP_VIDEO);
        apply_video_audio(false);

        field_.placeholder = L"Search images & videos";
        cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
        cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
        return true;
    }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool visible() const noexcept { return IsWindowVisible(hwnd_) != FALSE; }
    WindowTarget& target() noexcept { return target_; }

    void apply_theme() noexcept {
        target_.set_liquid_glass(glass_active());
        if (glass_active()) {
            if (visible() && !sh_.test_mode) SetTimer(hwnd_, TIMER_GLASS, 16, nullptr);
        } else {
            KillTimer(hwnd_, TIMER_GLASS);
            glass_.release();
            sh_.desktop_dupl.reset();
        }
        full_dirty_ = true;
        if (hwnd_ && visible() && !fading_out_) {
            const ShadowPad prev = applied_pad_;
            const ShadowPad next = active_pad();
            applied_pad_ = next;
            apply_window_theme(hwnd_, sh_.theme, true, !fullscreen_);
            if (!fullscreen_ && prev != next) {
                RECT wr{};
                GetWindowRect(hwnd_, &wr);
                const UINT dpi = GetDpiForWindow(hwnd_);
                const float s = dpi > 0 ? static_cast<float>(dpi) / 96.0f : target_.scale();
                const LONG pl = static_cast<LONG>(std::lround((next.left - prev.left) * s));
                const LONG pt = static_cast<LONG>(std::lround((next.top - prev.top) * s));
                const LONG pr = static_cast<LONG>(std::lround((next.right - prev.right) * s));
                const LONG pb = static_cast<LONG>(std::lround((next.bottom - prev.bottom) * s));
                SetWindowPos(hwnd_, nullptr, wr.left - pl, wr.top - pt,
                             (wr.right - wr.left) + pl + pr, (wr.bottom - wr.top) + pt + pb,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            } else {
                SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            invalidate();
        }
    }

    void show() {
        blur_hide_tick_ = 0;
        const bool was_visible = visible() && !fading_out_;
        apply_window_theme(hwnd_, sh_.theme, true, !fullscreen_);
        if (!was_visible || applied_pad_ != active_pad()) {
            place();
            if (!visible()) fade_ = sh_.test_mode ? 1.0f : 0.0f;
        }
        fading_out_ = false;
        target_fade_ = 1.0f;
        apply_fade();
        shown_tick_ = GetTickCount64();
        selecting_text_ = false;
        dragging_scroll_ = false;
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        if (glass_active()) ensure_window_capture_affinity(hwnd_, true);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        window_active_ = true;
        field_.focused = !preview_valid();
        if (field_.focused) field_.select_all();
        caret_reset();
        sh_.refresh_tags();
        sh_.poll_indexer(true);
        sh_.refresh_hotkey_state();
        if (!sh_.test_mode && sh_.on_ui_opened) sh_.on_ui_opened();
        if (preview_valid()) request_preview();
        SetTimer(hwnd_, TIMER_STATUS, 1000, nullptr);
        if (glass_active() && !sh_.test_mode) SetTimer(hwnd_, TIMER_GLASS, 16, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
        full_dirty_ = true;
        pending_dirty_.clear();
        last_tooltip_dirty_.reset();
        render();
        ValidateRect(hwnd_, nullptr);
        if (!sh_.test_mode && fade_ < 1.0f) {
            last_fade_ = std::chrono::steady_clock::now();
            SetTimer(hwnd_, TIMER_FADE, 15, nullptr);
        }
    }

    void finish_hide() {
        fading_out_ = false;
        fade_ = target_fade_ = 0.0f;
        chip_anim_.snap(static_cast<float>(chip_ + 1));
        searching_ = false;
        loading_more_ = false;
        debounce_pending_ = false;
        KillTimer(hwnd_, TIMER_CARET);
        KillTimer(hwnd_, TIMER_DEBOUNCE);
        KillTimer(hwnd_, TIMER_STATUS);
        KillTimer(hwnd_, TIMER_DEVICE);
        KillTimer(hwnd_, TIMER_FADE);
        KillTimer(hwnd_, TIMER_GIF);
        KillTimer(hwnd_, TIMER_GLASS);
        set_video_timer(false);
        dragging_seek_ = false;
        dragging_volume_ = false;
        gif_scrub_resume_ = false;
        pv_vol_rect_ = {};
        gif_.close();
        video_.close();
        sh_.previews.clear();
        anim_active_ = false;
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        hover_.set(IdNone, 0, false);
        card_hover_.set(IdNone, 0, false);
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        pending_dirty_.clear();
        last_tooltip_dirty_.reset();
        full_dirty_ = true;
        ShowWindow(hwnd_, SW_HIDE);
        ValidateRect(hwnd_, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
    }

    void hide() {
        if (!visible()) return;
        selecting_text_ = false;
        dragging_scroll_ = false;
        dragging_seek_ = false;
        dragging_volume_ = false;
        gif_scrub_resume_ = false;
        ReleaseCapture();
        close_popups();
        KillTimer(hwnd_, TIMER_CARET);
        KillTimer(hwnd_, TIMER_STATUS);
        KillTimer(hwnd_, TIMER_GIF);
        set_video_timer(false);
        gif_.set_playing(false);
        video_.suspend();
        if (sh_.test_mode) {
            finish_hide();
            return;
        }
        if (fading_out_) return;
        fading_out_ = true;
        target_fade_ = 0.0f;
        last_fade_ = std::chrono::steady_clock::now();
        SetTimer(hwnd_, TIMER_FADE, 15, nullptr);
    }

    /// Drops GPU resources bound to preview players on D3D/D2D device reset or adapter switch.
    void reset_preview_gpu() {
        gif_.reset_gpu();
        video_.reset_gpu();
    }

    /// Cancels and joins background clipboard copy workers before message-queue teardown.
    void stop_workers() noexcept {
        for (auto& [id, worker] : copy_workers_) worker.request_stop();
        copy_workers_.clear();
    }

    /// COM objects this window still holds; released before OLE is uninitialised.
    void release_com_objects() noexcept {
        async_drops_.clear();
        pending_clipboard_.reset();
        gif_.shutdown();
        video_.shutdown();
    }

    void toggle() {
        if (visible() && !fading_out_ && GetForegroundWindow() == hwnd_) {
            hide();
        } else {
            show();
        }
    }

    void tray_hide() {
        blur_hide_tick_ = 0;
        hide();
    }

    void tray_select(POINT pt) {
        if (visible()) {
            if (!fading_out_) hide();
            blur_hide_tick_ = 0;
            return;
        }
        // Fallback if WM_LBUTTONDOWN wasn't delivered before NIN_SELECT: don't re-open a pill that just hid from this click.
        const ULONGLONG blur_tick = std::exchange(blur_hide_tick_, 0);
        if (blur_tick != 0 && GetTickCount64() - blur_tick < 1500 &&
            std::abs(pt.x - blur_hide_cursor_.x) <= 48 && std::abs(pt.y - blur_hide_cursor_.y) <= 48) {
            return;
        }
        show();
    }

    /// Asks for a repaint. Any number of calls before the next WM_PAINT collapse into a single frame, and
    /// WM_PAINT is only delivered once pending input has been handled, so typing never waits on drawing.
    void invalidate() noexcept {
        dirty_ = true;
        full_dirty_ = true;
        if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    /// Draws a frame right now. Prefer `invalidate()`: presenting waits for the display's refresh.
    void render() {
        if (rendering_ || (!visible() && !sh_.test_mode)) return;
        if (!ensure_target()) return;
        if (!prepare_preview_media()) return;
        rendering_ = true;
        dirty_ = false;

        const float prev_scroll = scroll_;
        const float prev_chip = chip_anim_.value;
        if (sh_.test_mode || dragging_scroll_ || std::abs(target_scroll_ - scroll_) >= 0.5f ||
            chip_anim_.active() || searching_ || loading_more_ || preview_loading() || glass_scene_needed()) {
            full_dirty_ = true;
        }
        if (glass_active() && !full_dirty_ &&
            (!glass_.has_cached_desktop_glass() || glass_.poll_desktop(sh_.desktop_dupl, sh_.gfx, hwnd_))) {
            full_dirty_ = true;
        }

        dirty_dips_scratch_.clear();
        if (!full_dirty_) {
            dirty_dips_scratch_.swap(pending_dirty_);
            if (hover_.in_anim.value != hover_.in_anim.target) queue_hover_dirty(dirty_dips_scratch_, hover_.id, hover_.index, full_dirty_);
            if (hover_.prev_id != IdNone && hover_.out_anim.value != hover_.out_anim.target) {
                queue_hover_dirty(dirty_dips_scratch_, hover_.prev_id, hover_.prev_index, full_dirty_);
            }
            if (card_hover_.in_anim.value != card_hover_.in_anim.target) {
                queue_hover_dirty(dirty_dips_scratch_, card_hover_.id, card_hover_.index, full_dirty_);
            }
            if (card_hover_.prev_id != IdNone && card_hover_.out_anim.value != card_hover_.out_anim.target) {
                queue_hover_dirty(dirty_dips_scratch_, card_hover_.prev_id, card_hover_.prev_index, full_dirty_);
            }
        } else {
            pending_dirty_.clear();
        }

        const bool anim = step_anim();
        if (scroll_ != prev_scroll || chip_anim_.value != prev_chip) {
            full_dirty_ = true;
        }

        std::optional<D2D1_RECT_F> curr_tip_dirty;
        if (!menu_open_ && !drop_open_ && tooltip_.alpha.value > 0.008f &&
            (!tooltip_.title.empty() || !tooltip_.subtitle.empty())) {
            const TooltipLayout tl = measure_tooltip_layout(sh_.fonts, target_.size(), tooltip_.anchor, tooltip_.title, tooltip_.subtitle);
            if (tl.valid) curr_tip_dirty = tl.dirty;
        }
        if (!full_dirty_) {
            if (curr_tip_dirty) dirty_dips_scratch_.push_back(*curr_tip_dirty);
            if (last_tooltip_dirty_ && (!curr_tip_dirty || !rect_equal(*last_tooltip_dirty_, *curr_tip_dirty))) {
                dirty_dips_scratch_.push_back(*last_tooltip_dirty_);
            }
        }

        const bool request_full = std::exchange(full_dirty_, false);
        partial_hits_.clear();
        bool drew_full = false;
        const bool ok = target_.render_frame(request_full, dirty_dips_scratch_, !in_sizemove_, [&](std::optional<D2D1_RECT_F> clip, bool full_frame) {
            if (full_frame) {
                drew_full = true;
                hits_.clear();
                Canvas cv{target_, sh_.fonts, hits_, sh_.theme, clip};
                draw(cv);
            } else {
                Canvas cv{target_, sh_.fonts, partial_hits_, sh_.theme, clip};
                draw(cv);
            }
        });
        glass_.end_frame();
        if (!ok) recover();
        if (!drew_full && !partial_hits_.empty()) sync_card_overlay_hits(hits_, partial_hits_);
        last_tooltip_dirty_ = curr_tip_dirty;
        rendering_ = false;
        anim_active_ = visible() && (anim || searching_ || loading_more_);
    }

    /// Test hook: puts the pill into a given state without going through input.
    void test_set_query(std::wstring q) {
        field_.set(std::move(q));
        run_search(0);
    }
    void test_open_menu(int index) {
        if (std::cmp_greater_equal(index ,items_.size())) return;
        selected_ = {items_[index].pt.id};
        focus_ = index;
        open_menu(index, {.x=420.0f, .y=260.0f});
    }
    void test_dropdown(std::wstring q) {
        field_.set(std::move(q));
        update_dropdown();
    }
    void set_fullscreen(bool on) {
        if (on != fullscreen_) toggle_fullscreen();
    }
    /// Test hook: opens the viewer on a result (-1 closes it).
    void test_preview(int index) {
        if (index < 0) {
            close_preview();
        } else {
            open_preview(index);
        }
    }
    [[nodiscard]] bool preview_loading() const {
        if (!preview_valid()) return false;
        const Item& it = items_[preview_];
        if (it.pt.p.k == 1) return video_.is_loading();
        return !sh_.previews.failed(it.pt.id) && !sh_.previews.get(it.pt.id) && !gif_.has_frame();
    }
    [[nodiscard]] size_t result_count() const noexcept { return items_.size(); }
    [[nodiscard]] bool searching() const noexcept { return searching_; }

private:
    // ------------------------------------------------------------------------------------------
    // Layout constants (DIPs)
    // ------------------------------------------------------------------------------------------
    static constexpr float HEADER_H = 60.0f;
    static constexpr float STATUS_H = 30.0f;
    static constexpr float PAD = 14.0f;
    static constexpr float GAP = 12.0f;
    static constexpr float CARD_MIN_W = 228.0f;
    static constexpr float CARD_BODY_H = 124.0f;
    static constexpr double GRID_ZOOM_MIN = 1.0;
    static constexpr double GRID_ZOOM_MAX = 4.0;
    static constexpr float DEFAULT_W = 880.0f;
    static constexpr float DEFAULT_H = 640.0f;
    static constexpr float MENU_W = 256.0f;
    static constexpr UINT_PTR TIMER_CARET = 1, TIMER_DEBOUNCE = 2, TIMER_STATUS = 4, TIMER_DEVICE = 5, TIMER_FADE = 6, TIMER_DROP = 7, TIMER_COPY = 8, TIMER_TOOLTIP = 9, TIMER_GIF = 10, TIMER_VIDEO = 11, TIMER_GLASS = 12;
    static constexpr uint64_t COPY_MAX_PIXELS = 40'000'000;
    static constexpr int CLIPBOARD_ATTEMPTS = 10;

    enum Id : uint8_t {
        IdNone = 0,
        IdField,
        IdChip,
        IdTagBtn,
        IdNotifyBtn,
        IdStatusNotify,
        IdZoomReset,
        IdSettings,
        IdFullscreen,
        IdClose,
        IdCard,
        IdCardName,
        IdCardDir,
        IdSimilar,
        IdCardPreview,
        IdCardMoreTags,
        IdTagChip,
        IdScrollbar,
        IdMenu,
        IdMenuItem,
        IdMenuTag,
        IdMenuField,
        IdDrop,
        IdDropItem,
        IdHelpBtn,
        IdHelp,
        IdHelpItem,
        IdGrid,
        IdPreview,      // viewer backdrop
        IdPvImage,
        IdPvPrev,
        IdPvNext,
        IdPvPlay,
        IdPvMute,
        IdPvVolume,
        IdPvSeek,
        IdPvBar,
        IdPvOpen,
        IdPvReveal,
        IdPvCopy,
        IdPvClose,
        IdPvPath,
    };
    enum MenuAction : uint8_t { ActOpen, ActPreview, ActReveal, ActSimilar, ActCopy, ActCopyPath, ActCount };

    struct Item {
        DbPoint pt;
        std::wstring name;
        std::wstring dir;
        std::wstring meta;
        std::wstring date;
        std::wstring duration_w;
        std::wstring dir_disp;
        float dir_disp_w = -1.0f;
        std::vector<std::wstring> tags_w;
    };

    static Item make_item(DbPoint p) {
        Item it;
        const std::wstring path = to_wide(p.p.p);
        const auto slash = path.find_last_of(L'\\');
        it.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        it.dir = slash == std::wstring::npos ? L"" : path.substr(0, slash);
        if (p.p.w && p.p.h) {
            append_to(it.meta, to_fixed_wstr(p.p.w), L"×", to_fixed_wstr(p.p.h), L"  •  ", format_size(p.p.s));
        } else {
            it.meta = format_size(p.p.s).str();
        }
        it.date = format_date(p.p.m).str();
        if (p.p.k == 1 && p.p.d) it.duration_w = format_duration_ms(p.p.d).str();
        it.tags_w.reserve(p.p.t.size());
        for (const auto& t : p.p.t) it.tags_w.push_back(to_wide(t));
        it.pt = std::move(p);
        return it;
    }

    [[nodiscard]] static std::wstring item_path(const Item& it)  { return to_wide(it.pt.p.p); }

    // ------------------------------------------------------------------------------------------
    // Window placement
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] bool expanded() const noexcept { return !field_.text.empty() || !items_.empty() || fullscreen_; }

    [[nodiscard]] static UINT monitor_dpi(HMONITOR mon) noexcept {
        UINT dpi_x = 96;
        UINT dpi_y = 96;
        if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0) return 96;
        return dpi_x;
    }

    /// Moves/sizes the window ourselves for a known destination DPI; the WM_DPICHANGED this can trigger must not
    /// apply its suggested rect on top (that would scale the window a second time), and WM_GETMINMAXINFO must use
    /// the destination DPI rather than the previous monitor's DPI.
    void set_pos_for_dpi(HWND after, int x, int y, int w, int h, UINT flags, UINT dpi = 0) noexcept {
        placing_ = true;
        placing_dpi_ = dpi;
        SetWindowPos(hwnd_, after, x, y, w, h, flags);
        placing_ = false;
        placing_dpi_ = 0;
    }

    struct ShadowPad {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        [[nodiscard]] bool operator==(const ShadowPad&) const noexcept = default;
    };

    [[nodiscard]] bool custom_pill_frame() const noexcept {
        return !fullscreen_ && sh_.theme.pill_radius.has_value();
    }

    [[nodiscard]] ShadowPad active_pad() const noexcept {
        return custom_pill_frame() ? ShadowPad{.left = 24.0f, .top = 18.0f, .right = 24.0f, .bottom = 28.0f} : ShadowPad{};
    }

    [[nodiscard]] D2D1_RECT_F pill_bounds(D2D1_SIZE_F sz) const noexcept {
        const ShadowPad p = active_pad();
        return D2D1::RectF(p.left, p.top,
                           std::max(p.left + 1.0f, sz.width - p.right),
                           std::max(p.top + 1.0f, sz.height - p.bottom));
    }

    [[nodiscard]] float effective_pill_radius(D2D1_RECT_F pr) const noexcept {
        if (fullscreen_) return 0.0f;
        const float raw = sh_.theme.pill_radius.value_or(sh_.theme.round_corners ? 12.0f : 0.0f);
        return std::clamp(raw, 0.0f, std::min(width(pr), height(pr)) * 0.5f);
    }

    void place() noexcept {
        if (fullscreen_) return;
        POINT cur;
        GetCursorPos(&cur);
        HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
        const POINT saved{.x = sh_.settings.pill_x, .y = sh_.settings.pill_y};
        bool use_saved = sh_.settings.pill_x != INT32_MIN;
        const ShadowPad pad = active_pad();
        applied_pad_ = pad;
        const auto content_size_on = [&](HMONITOR m) noexcept {
            const float s = static_cast<float>(monitor_dpi(m)) / 96.0f;
            return SIZE{.cx = static_cast<LONG>((sh_.settings.pill_w > 0 ? static_cast<float>(sh_.settings.pill_w) : DEFAULT_W) * s),
                        .cy = static_cast<LONG>(current_height_dip() * s),};
        };
        if (use_saved) {
            // Windows gives a window the DPI of the monitor holding most of it, not the one under its top-left corner.
            HMONITOR guess = MonitorFromPoint(saved, MONITOR_DEFAULTTONULL);
            if (!guess) guess = MonitorFromPoint(saved, MONITOR_DEFAULTTONEAREST);
            const SIZE est = content_size_on(guess);
            const RECT predicted{.left = saved.x, .top = saved.y, .right = saved.x + est.cx, .bottom = saved.y + est.cy};
            if (HMONITOR m = MonitorFromRect(&predicted, MONITOR_DEFAULTTONULL)) {
                mon = m;
            } else {
                use_saved = false; // the monitor it was saved on is gone: re-centre on the cursor's monitor
            }
        }
        const UINT dpi = monitor_dpi(mon);
        const float s = static_cast<float>(dpi) / 96.0f;
        const SIZE csz = content_size_on(mon);
        const int pad_l = static_cast<int>(std::lround(pad.left * s));
        const int pad_t = static_cast<int>(std::lround(pad.top * s));
        const int pad_r = static_cast<int>(std::lround(pad.right * s));
        const int pad_b = static_cast<int>(std::lround(pad.bottom * s));
        MONITORINFO mi{.cbSize = sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        const RECT& wa = mi.rcWork;
        const int wa_w = std::max<int>(1, wa.right - wa.left);
        const int wa_h = std::max<int>(1, wa.bottom - wa.top);
        const int w = std::min<int>(csz.cx + pad_l + pad_r, wa_w);
        const int h = std::min<int>(csz.cy + pad_t + pad_b, wa_h);
        int x = (use_saved ? saved.x : wa.left + ((wa_w - csz.cx) / 2)) - pad_l;
        int y = (use_saved ? saved.y : wa.top + ((wa_h - csz.cy) / 5)) - pad_t;
        x = std::clamp<int>(x, wa.left, wa.right - w);
        y = std::clamp<int>(y, wa.top, wa.bottom - h);
        set_pos_for_dpi(HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE, dpi);
    }

    static constexpr float HELP_W = 420.0f;
    static constexpr float HELP_H = 240.0f;

    [[nodiscard]] bool show_help() const noexcept {
        return help_pinned_ || raw_hover_id_ == IdHelpBtn || raw_hover_id_ == IdHelp || raw_hover_id_ == IdHelpItem;
    }

    [[nodiscard]] static std::pair<int, int> card_hover_target(int id, int idx) noexcept {
        return (id >= IdCard && id <= IdTagChip) ? std::pair<int, int>{IdCard, idx} : std::pair<int, int>{IdNone, 0};
    }

    [[nodiscard]] static std::pair<int, int> visual_hover(int id, int idx) noexcept {
        switch (id) {
            case IdCardName:
            case IdCardDir:
            case IdCardMoreTags:
            case IdTagChip: return {IdCard, idx};
            case IdField:
            case IdGrid:
            case IdPreview:
            case IdPvImage:
            case IdPvPath:
            case IdPvBar:
            case IdMenu:
            case IdMenuField:
            case IdDrop:
            case IdHelp: return {IdNone, 0};
            default: return {id, idx};
        }
    }

    [[nodiscard]] static bool rect_equal(D2D1_RECT_F a, D2D1_RECT_F b) noexcept {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }

    void queue_hover_dirty(std::vector<D2D1_RECT_F>& out, int id, int idx, bool& force_full) const {
        if (id == IdNone) return;
        if (id == IdScrollbar) {
            if (scroll_thumb_.right > scroll_thumb_.left && scroll_thumb_.bottom > scroll_thumb_.top) {
                out.push_back(D2D1::RectF(scroll_thumb_.left - 4.0f, scroll_thumb_.top - 2.0f,
                                          scroll_thumb_.right + 2.0f, scroll_thumb_.bottom + 2.0f));
            }
            return;
        }
        if (id >= IdCard && id <= IdTagChip) {
            for (const Hit& h : hits_) {
                if (h.id == IdCard && h.index == idx) {
                    out.push_back(h.r);
                    return;
                }
            }
            force_full = true;
            return;
        }
        bool found = false;
        for (const Hit& h : hits_) {
            if (h.id == id && h.index == idx) {
                out.push_back(h.r);
                found = true;
            }
        }
        if (!found) force_full = true;
    }

    void queue_hover_dirty(int id, int idx) {
        queue_hover_dirty(pending_dirty_, id, idx, full_dirty_);
    }

    void sync_card_overlay_hits(std::vector<Hit>& hits, std::span<const Hit> partial_hits) {
        touched_cards_scratch_.clear();
        for (const Hit& h : partial_hits) {
            if (h.id == IdCard && std::ranges::find(touched_cards_scratch_, h.index) == touched_cards_scratch_.end()) {
                touched_cards_scratch_.push_back(h.index);
            }
        }
        if (touched_cards_scratch_.empty()) return;

        for (const int card_idx : touched_cards_scratch_) {
            std::erase_if(hits, [card_idx](const Hit& h) {
                return h.index == card_idx && (h.id == IdSimilar || h.id == IdCardPreview);
            });

            overlays_scratch_.clear();
            for (const Hit& ph : partial_hits) {
                if (ph.index == card_idx && (ph.id == IdSimilar || ph.id == IdCardPreview)) {
                    const bool dup = std::ranges::any_of(overlays_scratch_, [&](const Hit& ex) {
                        return ex.id == ph.id && ex.index == ph.index;
                    });
                    if (!dup) overlays_scratch_.push_back(ph);
                }
            }
            if (overlays_scratch_.empty()) continue;

            const auto card_it = std::ranges::find_if(hits, [card_idx](const Hit& h) {
                return h.id == IdCard && h.index == card_idx;
            });
            if (card_it != hits.end()) {
                hits.insert(std::next(card_it), overlays_scratch_.begin(), overlays_scratch_.end());
            }
        }
    }

    void clear_tooltip_now() {
        if (last_tooltip_dirty_ && tooltip_.alpha.value > 0.008f) {
            pending_dirty_.push_back(*last_tooltip_dirty_);
            if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
        }
        tooltip_.clear(hwnd_, TIMER_TOOLTIP);
    }

    void clear_hover(bool animate = false) {
        queue_hover_dirty(hover_.id, hover_.index);
        queue_hover_dirty(hover_.prev_id, hover_.prev_index);
        queue_hover_dirty(card_hover_.id, card_hover_.index);
        queue_hover_dirty(card_hover_.prev_id, card_hover_.prev_index);
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        hover_.set(IdNone, 0, animate);
        card_hover_.set(IdNone, 0, animate);
    }

    [[nodiscard]] float help_height_dip() const noexcept {
        if (!show_help()) return 0.0f;
        return (HEADER_H - 4.0f) + HELP_H + 12.0f;
    }

    [[nodiscard]] float dropdown_height_dip() const noexcept {
        if (!drop_open_) return 0.0f;
        const float row = 32.0f;
        const float h = drop_items_.empty() ? 44.0f : (8.0f + (row * static_cast<float>(drop_items_.size())));
        return (HEADER_H - 4.0f) + h + 12.0f;
    }

    [[nodiscard]] float current_height_dip() const noexcept {
        if (!expanded()) return std::max({HEADER_H + STATUS_H, dropdown_height_dip(), help_height_dip()});
        return sh_.settings.pill_h > 0 ? static_cast<float>(sh_.settings.pill_h) : DEFAULT_H;
    }

    void apply_height() noexcept {
        if (fullscreen_ || in_sizemove_ || rendering_) return;
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        const UINT dpi = GetDpiForWindow(hwnd_);
        const float s = dpi > 0 ? static_cast<float>(dpi) / 96.0f : target_.scale();
        const ShadowPad pad = active_pad();
        MONITORINFO mi{.cbSize = sizeof(mi)};
        int max_h = INT_MAX;
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            max_h = std::max<int>(1, mi.rcWork.bottom - mi.rcWork.top);
        }
        const int h = std::min(static_cast<int>(std::lround((current_height_dip() + pad.top + pad.bottom) * s)), max_h);
        if (wr.bottom - wr.top != h) SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void save_rect() {
        if (fullscreen_) return;
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        const float s = target_.scale();
        const ShadowPad pad = active_pad();
        sh_.settings.pill_x = wr.left + static_cast<int32_t>(std::lround(pad.left * s));
        sh_.settings.pill_y = wr.top + static_cast<int32_t>(std::lround(pad.top * s));
        sh_.settings.pill_w = std::max<int32_t>(1, static_cast<int32_t>(std::lround((static_cast<float>(wr.right - wr.left) / s) - pad.left - pad.right)));
        if (expanded()) {
            sh_.settings.pill_h = std::max<int32_t>(1, static_cast<int32_t>(std::lround((static_cast<float>(wr.bottom - wr.top) / s) - pad.top - pad.bottom)));
        }
        sh_.save();
    }

    void toggle_fullscreen() {
        if (!fullscreen_) {
            GetWindowRect(hwnd_, &restore_rect_);
            restore_dpi_ = GetDpiForWindow(hwnd_);
            const float rs = (restore_dpi_ > 0 ? static_cast<float>(restore_dpi_) : 96.0f) / 96.0f;
            restore_rect_.left += static_cast<LONG>(std::lround(applied_pad_.left * rs));
            restore_rect_.top += static_cast<LONG>(std::lround(applied_pad_.top * rs));
            restore_rect_.right -= static_cast<LONG>(std::lround(applied_pad_.right * rs));
            restore_rect_.bottom -= static_cast<LONG>(std::lround(applied_pad_.bottom * rs));
            HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{.cbSize = sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            fullscreen_ = true;
            applied_pad_ = active_pad();
            apply_window_theme(hwnd_, sh_.theme, true, false);
            const RECT& r = mi.rcMonitor;
            set_pos_for_dpi(HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED, monitor_dpi(mon));
        } else {
            fullscreen_ = false;
            applied_pad_ = active_pad();
            apply_window_theme(hwnd_, sh_.theme, true, true);
            // The restore rect was captured at `restore_dpi_`; rescale it if that monitor's scale changed meanwhile.
            const RECT& r = restore_rect_;
            HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
            const UINT dpi_now = monitor_dpi(mon);
            const float k = restore_dpi_ > 0 ? static_cast<float>(dpi_now) / static_cast<float>(restore_dpi_) : 1.0f;
            const float ns = static_cast<float>(dpi_now) / 96.0f;
            const int pad_l = static_cast<int>(std::lround(applied_pad_.left * ns));
            const int pad_t = static_cast<int>(std::lround(applied_pad_.top * ns));
            const int pad_r = static_cast<int>(std::lround(applied_pad_.right * ns));
            const int pad_b = static_cast<int>(std::lround(applied_pad_.bottom * ns));
            MONITORINFO mi{.cbSize = sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            const RECT& wa = mi.rcWork;
            const int wa_w = std::max<int>(1, wa.right - wa.left);
            const int wa_h = std::max<int>(1, wa.bottom - wa.top);
            const int w = std::min<int>(static_cast<int>(std::lround(static_cast<float>(r.right - r.left) * k)) + pad_l + pad_r, wa_w);
            const int h = std::min<int>(static_cast<int>(std::lround(static_cast<float>(r.bottom - r.top) * k)) + pad_t + pad_b, wa_h);
            const int x = std::clamp<int>(r.left - pad_l, wa.left, wa.right - w);
            const int y = std::clamp<int>(r.top - pad_t, wa.top, wa.bottom - h);
            set_pos_for_dpi(HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED, dpi_now);
            apply_height();
        }
        full_dirty_ = true;
        render();
    }

    // ------------------------------------------------------------------------------------------
    // Device handling
    // ------------------------------------------------------------------------------------------

    bool ensure_target() {
        if (target_.ready() && gfx_epoch_ == sh_.gfx.epoch()) return true;
        if (target_.ready() && gfx_epoch_ != sh_.gfx.epoch()) {
            target_.release();
            preview_gpu_reset_ = true;
        }
        if (!sh_.gfx.ready() || sh_.gfx.device_lost()) {
            reset_preview_gpu();
            preview_gpu_reset_ = true;
            if (!sh_.gfx.recover(hwnd_)) {
                SetTimer(hwnd_, TIMER_DEVICE, 250, nullptr);
                return false;
            }
        }
        if (!target_.rebuild()) {
            SetTimer(hwnd_, TIMER_DEVICE, 250, nullptr);
            return false;
        }
        gfx_epoch_ = sh_.gfx.epoch();
        if (deferred_thumbs_) {
            deferred_thumbs_ = false;
            (void)sh_.thumbs.process(target_.dc());
            const bool previews = sh_.previews.process(target_.dc());
            if (preview_ < 0 && previews) sh_.previews.clear();
        }
        if (std::exchange(preview_gpu_reset_, false) && preview_valid() && items_[preview_].pt.p.k != 1) {
            request_preview();
        }
        return true;
    }

    void recover() {
        // Present failed: device removed/reset or adapter gone. Rebuild everything on the next frame.
        reset_preview_gpu();
        preview_gpu_reset_ = preview_valid();
        glass_.release();
        sh_.desktop_dupl.reset();
        target_.release();
        sh_.gfx.recover(hwnd_);
        sh_.thumbs.reset_gpu();
        sh_.previews.reset_gpu();
        full_dirty_ = true;
        pending_dirty_.clear();
        last_tooltip_dirty_.reset();
        SetTimer(hwnd_, TIMER_DEVICE, 100, nullptr);
    }

    // ------------------------------------------------------------------------------------------
    // Search
    // ------------------------------------------------------------------------------------------

    void schedule_search() {
        update_dropdown();
        if (trim(field_.text).empty()) {
            run_search(0);
            return;
        }
        debounce_pending_ = true;
        apply_height();
        SetTimer(hwnd_, TIMER_DEBOUNCE, 140, nullptr);
    }

    void run_search(uint64_t offset) {
        KillTimer(hwnd_, TIMER_DEBOUNCE);
        debounce_pending_ = false;
        if (offset == 0) {
            const std::wstring q = trim(field_.text);
            if (q.empty()) {
                ++seq_;
                if (menu_open_) close_popups();
                items_.clear();
                pagination_.reset();
                selected_.clear();
                deferred_select_.reset();
                drag_pending_ = false;
                focus_ = -1;
                error_.clear();
                searching_ = false;
                scroll_ = target_scroll_ = 0.0f;
                apply_height();
                invalidate();
                return;
            }
            last_query_ = q;
            apply_height();
        }
        searching_ = true;
        SearchRequest r;
        r.seq = offset == 0 ? ++seq_ : seq_;
        r.query = last_query_;
        r.kind = chip_;
        r.offset = offset;
        loading_more_ = offset > 0;
        sh_.engine.search(std::move(r));
        invalidate();
    }

public:
    void on_result(std::unique_ptr<SearchResult> r) {
        if (r->seq != seq_) return;
        searching_ = false;
        loading_more_ = false;
        error_ = r->error;
        last_ms_ = r->ms;
        has_more_ = r->has_more;
        semantic_ = r->semantic;
        if (!r->append) {
            if (preview_ >= 0) close_preview(false);
            if (menu_open_) close_popups();
            items_.clear();
            selected_.clear();
            deferred_select_.reset();
            drag_pending_ = false;
            focus_ = -1;
            scroll_ = target_scroll_ = 0.0f;
        }
        const bool keep_more = pagination_.apply_page(
            r->points, r->append, r->error.empty(), r->next_offset,
            [](const DbPoint& p) { return p.id; },
            [&](DbPoint&& p) { items_.push_back(make_item(std::move(p))); });
        if (!keep_more) has_more_ = false;
        apply_height();
        invalidate();
    }

    void on_thumbs() {
        if (!target_.ready()) {
            deferred_thumbs_ = true;
            return;
        }
        deferred_thumbs_ = false;
        const bool thumbs = sh_.thumbs.process(target_.dc());
        const bool previews = sh_.previews.process(target_.dc());
        // A full-size decode that lands after the viewer closed is not kept around
        if (preview_ < 0 && previews) sh_.previews.clear();
        if (thumbs || (previews && preview_ >= 0)) invalidate();
    }

    void on_indexer_changed() {
        sh_.poll_indexer(true);
        sh_.refresh_tags();
        if (visible()) invalidate();
    }

    /// The text model finished (re)loading: a query that failed for the lack of one is run again.
    void on_encoder_changed() {
        if (sh_.engine.encoder().state() == TextEncoder::State::Ready && !error_.empty() && !searching_ && !trim(field_.text).empty()) {
            run_search(0);
        }
        invalidate();
    }

    void on_tags_refreshed() {
        if (drop_open_) update_dropdown();
        if (visible()) invalidate();
    }

private:
    // ------------------------------------------------------------------------------------------
    // Grid geometry
    // ------------------------------------------------------------------------------------------

    struct Grid {
        int cols = 1;
        float card_w = 0, card_h = 0, thumb_h = 0;
        D2D1_RECT_F view{};
        float content_h = 0;
    };

    [[nodiscard]] double effective_grid_zoom() const noexcept {
        return std::clamp(sh_.settings.grid_zoom, GRID_ZOOM_MIN, GRID_ZOOM_MAX);
    }

    [[nodiscard]] bool is_grid_zoomed() const noexcept {
        return effective_grid_zoom() > GRID_ZOOM_MIN + 0.005;
    }

    [[nodiscard]] static int cols_for_zoom(float avail, double zoom) noexcept {
        const float min_w = CARD_MIN_W * static_cast<float>(std::clamp(zoom, GRID_ZOOM_MIN, GRID_ZOOM_MAX));
        return std::max(1, static_cast<int>((avail + GAP) / (min_w + GAP)));
    }

    [[nodiscard]] Grid grid() const noexcept {
        Grid g;
        const D2D1_RECT_F pr = pill_bounds(target_.size());
        g.view = D2D1::RectF(pr.left, pr.top + HEADER_H + STATUS_H + 1.0f, pr.right, pr.bottom);
        const float avail = width(pr) - (PAD * 2);
        g.cols = cols_for_zoom(avail, effective_grid_zoom());
        g.card_w = std::floor((avail - (GAP * static_cast<float>(g.cols - 1))) / static_cast<float>(g.cols));
        g.thumb_h = std::round((g.card_w - 12.0f) * 10.0f / 16.0f);
        g.card_h = g.thumb_h + CARD_BODY_H;
        const int rows = (static_cast<int>(items_.size()) + g.cols - 1) / g.cols;
        g.content_h = PAD + (static_cast<float>(rows) * (g.card_h + GAP)) - (rows != 0 ? GAP : 0.0f) + PAD + (has_more_ ? 40.0f : 0.0f);
        return g;
    }

    void set_grid_zoom(double next_zoom) {
        next_zoom = std::clamp(next_zoom, GRID_ZOOM_MIN, GRID_ZOOM_MAX);
        if (next_zoom < GRID_ZOOM_MIN + 0.02) next_zoom = GRID_ZOOM_MIN;
        if (std::abs(next_zoom - effective_grid_zoom()) < 0.001) return;

        const Grid old_g = grid();
        const int top_row = std::max(0, static_cast<int>(std::floor((scroll_ - PAD) / (old_g.card_h + GAP))));
        const float row_y_old = static_cast<float>(top_row) * (old_g.card_h + GAP);
        const float in_row_frac = (scroll_ > 0.0f && old_g.card_h + GAP > 0.0f)
            ? std::clamp((scroll_ - PAD - row_y_old) / (old_g.card_h + GAP), 0.0f, 1.0f)
            : 0.0f;
        const int anchor_idx = items_.empty() ? 0 : std::clamp(top_row * old_g.cols, 0, static_cast<int>(items_.size()) - 1);

        sh_.settings.grid_zoom = next_zoom;
        sh_.save();

        if (!items_.empty() && scroll_ > 0.0f) {
            const Grid new_g = grid();
            const int new_row = anchor_idx / new_g.cols;
            scroll_ = target_scroll_ = std::clamp(PAD + ((static_cast<float>(new_row) + in_row_frac) * (new_g.card_h + GAP)), 0.0f, max_scroll());
        } else {
            scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
        }
        clear_hover(false);
        full_dirty_ = true;
        invalidate();
    }

    void step_grid_zoom(int dir) {
        const float avail = std::max(1.0f, width(pill_bounds(target_.size())) - (PAD * 2.0f));
        const Grid g = grid();
        if (dir > 0) {
            if (g.cols <= 1) return;
            const double min_w = ((static_cast<double>(avail) + GAP) / static_cast<double>(g.cols)) - GAP + 1.0;
            double next_z = std::ceil((min_w / CARD_MIN_W) * 100.0) / 100.0;
            next_z = std::max(next_z, effective_grid_zoom() + 0.05);
            if (next_z > GRID_ZOOM_MAX) return;
            set_grid_zoom(next_z);
        } else if (dir < 0) {
            if (!is_grid_zoomed()) return;
            const int default_cols = cols_for_zoom(avail, GRID_ZOOM_MIN);
            const int target_cols = g.cols + 1;
            if (target_cols >= default_cols) {
                set_grid_zoom(GRID_ZOOM_MIN);
                return;
            }
            const double min_w = ((static_cast<double>(avail) + GAP) / (static_cast<double>(target_cols) + 0.5)) - GAP;
            const double next_z = std::clamp(min_w / CARD_MIN_W, GRID_ZOOM_MIN, effective_grid_zoom() - 0.01);
            set_grid_zoom(next_z);
        }
    }

    [[nodiscard]] float max_scroll() const noexcept {
        const Grid g = grid();
        return std::max(0.0f, g.content_h - height(g.view));
    }

    [[nodiscard]] D2D1_RECT_F card_rect(const Grid& g, int i) const noexcept {
        const int row = i / g.cols;
        const int col = i % g.cols;
        const float x = g.view.left + PAD + (static_cast<float>(col) * (g.card_w + GAP));
        const float y = g.view.top + PAD + (static_cast<float>(row) * (g.card_h + GAP)) - scroll_;
        return rect(x, y, g.card_w, g.card_h);
    }

    void scroll_to(int i, bool animate = true) {
        if (i < 0) return;
        const Grid g = grid();
        const D2D1_RECT_F r = card_rect(g, i);
        if (r.top < g.view.top + 4) {
            target_scroll_ = scroll_ + (r.top - g.view.top - PAD);
        } else if (r.bottom > g.view.bottom - 4) {
            target_scroll_ = scroll_ + (r.bottom - g.view.bottom + PAD);
        }
        target_scroll_ = std::clamp(target_scroll_, 0.0f, max_scroll());
        if (animate) {
            start_anim();
        } else {
            scroll_ = target_scroll_;
        }
    }

    void apply_fade() noexcept {
        const BYTE a = static_cast<BYTE>(std::round(std::clamp(fade_, 0.0f, 1.0f) * 255.0f));
        if (a != last_alpha_) {
            last_alpha_ = a;
            SetLayeredWindowAttributes(hwnd_, 0, a, LWA_ALPHA);
        }
    }

    void on_fade_timer() {
        const auto now = std::chrono::steady_clock::now();
        const float raw_dt = std::chrono::duration<float>(now - last_fade_).count();
        if (raw_dt <= 0.0f) return;
        const float dt = std::min(raw_dt, 0.05f);
        last_fade_ = now;
        const float step = dt / 0.11f;
        if (target_fade_ > fade_) {
            fade_ = std::min(target_fade_, fade_ + step);
        } else {
            fade_ = std::max(target_fade_, fade_ - step);
        }
        apply_fade();
        if (std::abs(target_fade_ - fade_) < 0.005f) {
            fade_ = target_fade_;
            apply_fade();
            KillTimer(hwnd_, TIMER_FADE);
            if (fading_out_) finish_hide();
        }
    }

    void start_anim() noexcept {
        if (!visible()) return;
        if (!anim_active_) last_anim_ = std::chrono::steady_clock::now();
        anim_active_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool step_anim() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_anim_).count(), 0.001f, 0.05f);
        last_anim_ = now;
        bool moving = false;
        const float d = target_scroll_ - scroll_;
        if (std::abs(d) < 0.5f) {
            scroll_ = target_scroll_;
        } else {
            scroll_ += d * (1.0f - std::exp(-dt * 22.0f));
            if (std::abs(target_scroll_ - scroll_) < 0.5f) {
                scroll_ = target_scroll_;
            } else {
                moving = true;
            }
        }
        if (chip_anim_.step(dt)) moving = true;
        if (hover_.step(dt)) moving = true;
        if (card_hover_.step(dt)) moving = true;
        if (tooltip_.step(dt)) moving = true;
        return moving;
    }

    // ------------------------------------------------------------------------------------------
    // Drawing
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] bool tooltip_for(int id, int idx, std::wstring& title, std::wstring& subtitle) const {
        title.clear();
        subtitle.clear();
        switch (id) {
            case IdTagBtn: title = L"Filter by tag"; return true;
            case IdZoomReset: {
                title = L"Reset grid zoom (Ctrl+0)";
                std::array<wchar_t, 48> zbuf; // NOLINT(cppcoreguidelines-pro-type-member-init)
                swprintf_s(zbuf.data(), zbuf.size(), L"Currently %.2gx — click to reset", effective_grid_zoom());
                subtitle = zbuf.data();
                return true;
            }
            case IdNotifyBtn:
            case IdStatusNotify:
                title = L"View notifications";
                subtitle = ulp::str::concat_strings(
                    format_count(sh_.notifications.size()),
                    sh_.notifications.size() == 1 ? L" active notification" : L" active notifications");
                return true;
            case IdHelpBtn: title = L"Search syntax & shortcuts"; return true;
            case IdSettings: title = L"Settings"; return true;
            case IdFullscreen: title = fullscreen_ ? L"Exit fullscreen (Esc)" : L"Fullscreen"; return true;
            case IdClose: title = L"Hide (Esc)"; return true;
            case IdCardPreview:
                title = (parse_thumb_dblclick(sh_.settings.thumb_dblclick) == ThumbDblClick::Preview)
                            ? L"Open in default app (Enter)"
                            : L"Preview (Space)";
                return true;
            case IdSimilar: title = L"Find similar (Ctrl+S)"; return true;
            case IdCardName:
            case IdCardDir:
                if (idx >= 0 && std::cmp_less(idx, items_.size())) {
                    title = items_[idx].name;
                    subtitle = items_[idx].dir;
                    return !title.empty() || !subtitle.empty();
                }
                return false;
            case IdCardMoreTags:
                if (idx >= 0 && std::cmp_less(idx, items_.size())) {
                    for (size_t k = 0; k < items_[idx].tags_w.size(); ++k) {
                        if (k) title += L", ";
                        title += items_[idx].tags_w[k];
                    }
                    return !title.empty();
                }
                return false;
            case IdPvPlay:
                if (preview_valid()) {
                    const Item& it = items_[preview_];
                    if (it.pt.p.k == 1 && !video_.failed()) {
                        title = video_.want_play() ? L"Pause (K)" : L"Play (K)";
                        return true;
                    }
                    if (gif_.has_animation()) {
                        title = (dragging_seek_ ? gif_scrub_resume_ : gif_.playing()) ? L"Pause (K)" : L"Play (K)";
                        return true;
                    }
                }
                title = L"Open video";
                return true;
            case IdPvMute: {
                const int pct = static_cast<int>(std::lround(displayed_video_volume() * 100.0f));
                title = effective_video_muted() ? L"Unmute (M)" : L"Mute (M)";
                subtitle = ulp::str::concat_strings(L"Volume: ", to_fixed_wstr(pct), L"%  \u2022  Scroll to adjust");
                return true;
            }
            case IdPvVolume: {
                const int pct = static_cast<int>(std::lround(displayed_video_volume() * 100.0f));
                title = ulp::str::concat_strings(L"Volume: ", to_fixed_wstr(pct), L"%");
                subtitle = L"Drag, scroll, or +/- keys";
                return true;
            }
            case IdPvPrev: title = L"Previous (Left)"; return true;
            case IdPvNext: title = L"Next (Right)"; return true;
            case IdPvOpen: title = L"Open (Enter)"; return true;
            case IdPvReveal: title = L"Show in Explorer (Ctrl+E)"; return true;
            case IdPvCopy: title = L"Copy media (Ctrl+C)"; return true;
            case IdPvClose: title = L"Close preview (Esc)"; return true;
            case IdPvPath:
                if (preview_valid()) {
                    title = items_[preview_].name;
                    subtitle = items_[preview_].dir;
                    return !title.empty() || !subtitle.empty();
                }
                return false;
            default: return false;
        }
    }

    void draw_active_tooltip(Canvas& cv, D2D1_SIZE_F sz) const {
        if (menu_open_ || drop_open_) return;
        draw_tooltip(cv, sz, tooltip_.anchor, tooltip_.title, tooltip_.subtitle, tooltip_.alpha.value);
    }

    [[nodiscard]] bool glass_active() const noexcept { return sh_.theme.is_liquid_glass; }
    [[nodiscard]] bool glass_scene_needed() const noexcept {
        return glass_active() && (preview_ >= 0 || menu_open_ || drop_open_ || show_help());
    }

    static void draw_pill_shadow(Canvas& cv, D2D1_RECT_F pr, float radius, bool focused) noexcept {
        const float theme_s = cv.theme().shadow_alpha_scale;
        if (theme_s <= 0.002f) return;
        if (const auto dc_clip = cv.dirty_clip()) {
            const D2D1_RECT_F inner = inset(pr, 0.5f, 0.5f);
            if (rect_inside_rounded_rect(*dc_clip, inner, std::max(0.0f, radius - 0.5f))) {
                return;
            }
        }
        constexpr int steps = 14;
        const float spread = focused ? 22.0f : 15.0f;
        const float dy_max = focused ? 4.0f : 2.0f;
        const float intensity = (focused ? 0.78f : 0.34f) * theme_s;
        const float max_d = spread * 0.80f;
        const D2D1_RECT_F bounds = D2D1::RectF(
            pr.left - max_d - 1.0f,
            pr.top - max_d - 1.0f,
            pr.right + max_d + 1.0f,
            pr.bottom + max_d + dy_max + 1.0f);
        if (!cv.intersects_clip(bounds, 1.0f)) return;

        for (int i = steps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float g = spread * t;
            const float d = g * 0.80f;
            const float dy = dy_max * t;
            const float falloff = (1.0f - t) * (1.0f - t);
            const float a = std::clamp(intensity * (0.045f + (0.085f * falloff)), 0.0f, 0.9f);
            cv.fill_round(
                D2D1::RectF(pr.left - d, pr.top - d + dy,
                            pr.right + d, pr.bottom + d + dy),
                radius > 0.0f ? (radius + d) : (g * 0.65f),
                hex(0x000000, a));
        }
        cv.fill_round(inset(pr, -0.75f, -0.75f), radius > 0.0f ? (radius + 0.75f) : 0.75f,
                      hex(0x000000, std::clamp(0.18f * (focused ? 1.0f : 0.55f) * theme_s, 0.0f, 0.6f)));
        // Punch out the pill interior so translucent themes/glass never darken from the shadow fill underneath.
        ID2D1DeviceContext6* dc = cv.dc();
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        cv.fill_round(inset(pr, 0.5f, 0.5f), std::max(0.0f, radius - 0.5f), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        const D2D1_RECT_F pr = pill_bounds(sz);
        const bool custom_frame = custom_pill_frame();
        const float outer_r = effective_pill_radius(pr);
        if (custom_frame) {
            const bool focused = window_active_ || (hwnd_ && GetForegroundWindow() == hwnd_);
            draw_pill_shadow(cv, pr, outer_r, focused);
        }

        const bool glass = glass_active();
        const bool need_scene = glass_scene_needed();
        const bool has_scene = need_scene && glass_.begin_scene(target_, sh_.gfx);
        if (glass) {
            if (!glass_.draw_desktop_glass(sh_.desktop_dupl, cv, target_, sh_.gfx, hwnd_, pr,
                                           make_desktop_glass_style(cv.theme(), outer_r, fullscreen_))) {
                if (custom_frame) {
                    cv.fill_round(pr, outer_r, with_alpha(cv.theme().bg, cv.theme().pill_bg_alpha));
                } else {
                    cv.fill(pr, with_alpha(cv.theme().bg, cv.theme().pill_bg_alpha));
                }
            }
        } else if (custom_frame) {
            cv.fill_round(pr, outer_r, with_alpha(cv.theme().bg, cv.theme().pill_bg_alpha));
        } else {
            cv.fill(pr, with_alpha(cv.theme().bg, cv.theme().pill_bg_alpha));
        }
        if (preview_ >= 0) {
            draw_preview(cv, pr, custom_frame, outer_r);
            if (custom_frame && !glass && cv.theme().pill_border.a > 0.0f) {
                cv.stroke_round(pr, outer_r, cv.theme().pill_border, 1.0f);
            }
            if (menu_open_) draw_menu(cv);
            draw_active_tooltip(cv, sz);
            return;
        }
        if (has_scene) {
            if (expanded()) {
                const bool clipped = custom_frame && cv.push_round_clip(pr, outer_r);
                draw_grid(cv);
                if (custom_frame) cv.pop_round_clip(clipped);
            }
            glass_.end_scene(target_);
            const bool clipped = custom_frame && cv.push_round_clip(pr, outer_r);
            draw_header(cv, pr);
            draw_status(cv, pr);
            if (custom_frame) cv.pop_round_clip(clipped);
        } else {
            const bool clipped = custom_frame && cv.push_round_clip(pr, outer_r);
            if (expanded()) draw_grid(cv);
            draw_header(cv, pr);
            draw_status(cv, pr);
            if (custom_frame) cv.pop_round_clip(clipped);
        }
        if (custom_frame && !glass && cv.theme().pill_border.a > 0.0f) {
            cv.stroke_round(pr, outer_r, cv.theme().pill_border, 1.0f);
        }
        if (drop_open_) draw_dropdown(cv, pr);
        if (show_help()) draw_help(cv, pr);
        if (menu_open_) draw_menu(cv);
        draw_active_tooltip(cv, sz);
    }

    void icon_button(Canvas& cv, D2D1_RECT_F r, wchar_t g, int id, bool active = false) {
        const float ha = hover_.alpha(id);
        const float btn_r = cv.theme().button_radius;
        const float rad = std::min(height(r) * 0.5f, btn_r <= 0.0f ? 0.0f : (btn_r + 8.0f));
        if (is_liquid_glass(cv.theme())) {
            const bool dark = cv.theme().dark_mode;
            if (active || ha > 0.002f) {
                const float op = active ? 1.0f : ha;
                const D2D1_COLOR_F tint = active
                    ? with_alpha(cv.theme().primary, dark ? 0.28f : 0.24f)
                    : with_alpha(cv.theme().surface_high, dark ? 0.30f : 0.42f);
                const bool use_scene = (preview_ >= 0);
                const GlassStyle gs = make_control_glass_style(
                    cv.theme(), rad, tint, op, use_scene ? 1.2f : 0.0f, 8.0f, 1.10f);
                const bool drew = use_scene
                    ? glass_.draw_scene_glass(cv, target_, sh_.gfx, r, gs)
                    : glass_.draw_cached_glass(cv, target_, sh_.gfx, r, gs);
                if (!drew) {
                    const D2D1_COLOR_F border = active
                        ? with_alpha(cv.theme().primary, dark ? 0.62f : 0.55f)
                        : with_alpha(hex(0xffffff), (dark ? 0.28f : 0.68f) * op);
                    draw_glass_box(cv, r, rad, tint, border, 1.0f, (dark ? 0.08f : 0.30f) * op);
                } else {
                    cv.stroke_round(r, rad,
                                    with_alpha(hex(0x000000), (dark ? 0.28f : 0.10f) * op), 1.0f);
                    if (active) {
                        cv.stroke_round(r, rad, with_alpha(cv.theme().primary, dark ? 0.58f : 0.52f), 1.0f);
                    }
                }
            }
        } else {
            if (active) {
                cv.fill_round(r, rad, with_alpha(cv.theme().primary, 0.16f));
            } else if (ha > 0.002f) {
                cv.fill_round(r, rad, with_alpha(cv.theme().on_surface, 0.08f * ha));
            }
        }
        cv.icon(g, r, active ? cv.theme().primary : lerp_color(cv.theme().on_surface_variant, cv.theme().on_surface, ha));
        cv.hit(r, id);
    }

    void draw_header(Canvas& cv, D2D1_RECT_F pr) {
        const float cy = pr.top + (HEADER_H / 2);
        // Search glyph with a status dot
        const D2D1_RECT_F ic = rect(pr.left + 16, cy - 12, 24, 24);
        cv.icon(glyph::search, ic, cv.theme().on_surface_variant, Font::IconLarge);
        const D2D1_COLOR_F dot = status_dot_color();
        cv.fill_circle({.x = ic.left + 20, .y = ic.top + 20}, 4.5f, cv.theme().bg);
        cv.fill_circle({.x = ic.left + 20, .y = ic.top + 20}, 3.0f, dot);

        // Right cluster: close, fullscreen, settings | tag, chips
        float x = pr.right - 12;
        const auto btn = [&](wchar_t g, int id, bool active = false) {
            x -= 36;
            icon_button(cv, rect(x, cy - 18, 36, 36), g, id, active);
        };
        btn(glyph::close, IdClose);
        btn(fullscreen_ ? glyph::exit_fullscreen : glyph::fullscreen, IdFullscreen);
        btn(glyph::settings, IdSettings);
        const float btn_r = cv.theme().button_radius;
        if (!sh_.notifications.empty()) {
            const bool has_err = sh_.error_notification_count() > 0;
            const D2D1_COLOR_F ncol = has_err ? cv.theme().error : cv.theme().amber;
            std::array<wchar_t, 32> cnt_buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
            const std::wstring_view cnt = format_count_to(sh_.notifications.size(), cnt_buf);
            const float bw = std::max(44.0f, 26.0f + cv.measure(cnt, Font::Caption) + 12.0f);
            x -= bw + 4;
            const D2D1_RECT_F nr = rect(x, cy - 15, bw, 30);
            const float nr_rad = std::min(15.0f, offset_radius(btn_r, 5.0f));
            const float ha = hover_.alpha(IdNotifyBtn);
            if (is_liquid_glass(cv.theme())) {
                const GlassStyle gs = make_control_glass_style(
                    cv.theme(), nr_rad, with_alpha(ncol, lerp(0.20f, 0.34f, ha)), 1.0f, 0.0f, 8.0f, 1.08f);
                if (!glass_.draw_cached_glass(cv, target_, sh_.gfx, nr, gs)) {
                    draw_glass_box(cv, nr, nr_rad, with_alpha(ncol, lerp(0.18f, 0.30f, ha)),
                                   with_alpha(ncol, lerp(0.45f, 0.75f, ha)));
                } else {
                    cv.stroke_round(nr, nr_rad, with_alpha(ncol, lerp(0.40f, 0.70f, ha)), 1.0f);
                }
            } else {
                cv.fill_round(nr, nr_rad, lerp_color(with_alpha(ncol, 0.16f), with_alpha(ncol, 0.28f), ha));
                cv.stroke_round(nr, nr_rad, with_alpha(ncol, lerp(0.45f, 0.75f, ha)));
            }
            cv.icon(glyph::warning, rect(nr.left + 8, nr.top, 16, 30), ncol, Font::IconSmall);
            cv.text(cnt, rect(nr.left + 25, nr.top, bw - 31, 30), Font::Caption, ncol);
            cv.hit(nr, IdNotifyBtn);
        }
        x -= 8;
        cv.vline(x, cy - 12, cy + 12, cv.theme().outline_variant);
        x -= 4;
        btn(glyph::help, IdHelpBtn, show_help());
        help_btn_x_ = x;
        btn(glyph::tag, IdTagBtn, drop_open_ && drop_from_button_);
        x -= 8;

        // Segmented filter chips
        static constexpr std::array<const wchar_t*, 3> labels = {L"All", L"Images", L"Videos"};
        std::array<float, 3> widths; // NOLINT(cppcoreguidelines-pro-type-member-init)
        float total = 6;
        for (size_t i = 0; i < labels.size(); ++i) {
            widths[i] = cv.measure(labels[i], Font::SmallSemi) + 26;
            total += widths[i];
        }
        x -= total;
        const D2D1_RECT_F seg = rect(x, cy - 17, total, 34);
        const float seg_rad = std::min(17.0f, offset_radius(btn_r, 7.0f));
        const float chip_rad = std::min(14.0f, offset_radius(btn_r, 4.0f));
        const bool glass_ui = is_liquid_glass(cv.theme());
        const bool dark = cv.theme().dark_mode;
        if (glass_ui) {
            const GlassStyle seg_gs = make_control_glass_style(
                cv.theme(), seg_rad, with_alpha(cv.theme().surface_high, dark ? 0.24f : 0.34f), 1.0f, 0.0f, 10.0f, 1.05f);
            if (!glass_.draw_cached_glass(cv, target_, sh_.gfx, seg, seg_gs)) {
                draw_glass_box(cv, seg, seg_rad,
                               with_alpha(cv.theme().surface_high, dark ? 0.26f : 0.36f),
                               with_alpha(hex(0xffffff), dark ? 0.20f : 0.62f));
            } else {
                cv.stroke_round(seg, seg_rad, with_alpha(hex(0x000000), dark ? 0.24f : 0.08f), 1.0f);
            }
        } else {
            cv.fill_round(seg, seg_rad, with_alpha(cv.theme().surface_highest, 0.85f));
        }
        std::array<D2D1_RECT_F, 3> chip_rects; // NOLINT(cppcoreguidelines-pro-type-member-init)
        float cx = x + 3;
        for (size_t i = 0; i < labels.size(); ++i) {
            chip_rects[i] = rect(cx, cy - 14, widths[i], 28);
            cx += widths[i];
        }
        const float ct = std::clamp(chip_anim_.value, 0.0f, 2.0f);
        for (int i = 0; i < 3; ++i) {
            const float act = std::max(0.0f, 1.0f - std::abs(ct - static_cast<float>(i)));
            const float ha = hover_.alpha(IdChip, i) * indicator_hover_fade(act);
            if (ha > 0.002f) {
                if (glass_ui) {
                    const GlassStyle h_gs = make_control_glass_style(
                        cv.theme(), chip_rad, with_alpha(hex(0xffffff), dark ? 0.12f : 0.34f), ha, 0.0f, 7.0f, 1.06f);
                    if (!glass_.draw_cached_glass(cv, target_, sh_.gfx, chip_rects[i], h_gs)) {
                        cv.fill_round(chip_rects[i], chip_rad, with_alpha(hex(0xffffff), (dark ? 0.10f : 0.32f) * ha));
                    }
                } else {
                    cv.fill_round(chip_rects[i], chip_rad, with_alpha(cv.theme().on_surface, 0.07f * ha));
                }
            }
        }
        const int lo = std::clamp(static_cast<int>(std::floor(ct)), 0, 1);
        const float frac = ct - static_cast<float>(lo);
        const D2D1_RECT_F active_chip = lerp_rect(chip_rects[lo], chip_rects[lo + 1], frac);
        if (glass_ui) {
            const GlassStyle act_gs = make_control_glass_style(
                cv.theme(), chip_rad, with_alpha(cv.theme().primary, dark ? 0.34f : 0.28f), 1.0f, 0.0f, 9.0f, 1.12f);
            if (!glass_.draw_cached_glass(cv, target_, sh_.gfx, active_chip, act_gs)) {
                draw_glass_box(cv, active_chip, chip_rad,
                               with_alpha(cv.theme().primary, dark ? 0.28f : 0.22f),
                               with_alpha(cv.theme().primary, dark ? 0.65f : 0.58f));
            } else {
                cv.stroke_round(active_chip, chip_rad, with_alpha(cv.theme().primary, dark ? 0.55f : 0.50f), 1.0f);
            }
        } else {
            cv.fill_round(active_chip, chip_rad, cv.theme().primary_container);
        }
        for (int i = 0; i < 3; ++i) {
            const float act = std::max(0.0f, 1.0f - std::abs(ct - static_cast<float>(i)));
            cv.text(labels[i], chip_rects[i], Font::SmallSemi,
                    lerp_color(cv.theme().on_surface_variant, cv.theme().on_primary_container, act), Align::Center);
            cv.hit(chip_rects[i], IdChip, i);
        }

        // Text field
        const D2D1_RECT_F fr = D2D1::RectF(pr.left + 52, pr.top + 8, x - 14, pr.top + HEADER_H - 8);
        cv.hit(fr, IdField);
        field_.draw(cv, D2D1::RectF(fr.left, fr.top, fr.right, fr.bottom), Font::Input, cv.theme().on_surface, cv.theme().outline);
        cv.hline(pr.left, pr.right, pr.top + HEADER_H, with_alpha(cv.theme().outline_variant, 0.7f));
    }

    [[nodiscard]] D2D1_COLOR_F status_dot_color() const noexcept {
        if (sh_.indexer_running || sh_.engine.db_paused_for_indexing()) return sh_.theme.amber;
        const auto st = sh_.engine.encoder().state();
        const auto db = sh_.engine.db_state();
        if (st == TextEncoder::State::Failed || !error_.empty() || (db == Engine::DbState::Failed && !sh_.test_mode)) return sh_.theme.error;
        if (st == TextEncoder::State::Loading || st == TextEncoder::State::Exporting || (db == Engine::DbState::Loading && !sh_.test_mode)) {
            return sh_.theme.primary;
        }
        return sh_.theme.mint;
    }

    void draw_status(Canvas& cv, D2D1_RECT_F pr) {
        const float y = pr.top + HEADER_H + 1;
        float x = pr.left + 18;
        auto piece = [&](std::wstring_view s, D2D1_COLOR_F c, Font f = Font::Caption) {
            if (s.empty()) return;
            const float w = cv.measure(s, f);
            cv.text(s, rect(x, y, w + 2, STATUS_H), f, c);
            x += w;
        };
        const auto sep = [&] { piece(L"   •   ", cv.theme().outline_variant); };

        cv.icon(glyph::clock, rect(x - 2, y + 8, 14, 14), cv.theme().outline, Font::IconSmall);
        x += 16;
        if (sh_.indexer_running) {
            const auto& d = sh_.indexer;
            if (sh_.indexer_stopping) {
                piece(L"Stopping indexer…", cv.theme().amber, Font::SmallSemi);
            } else {
                piece(L"Indexing ", cv.theme().outline);
                if (d.phase == Phase::Embedding) {
                    FixedWString<64> counts;
                    counts.assign(format_count(d.embedded));
                    counts.append(L" / ");
                    counts.append(format_count(d.found));
                    piece(counts, cv.theme().amber, Font::SmallSemi);
                } else {
                    piece(phase_label(d.phase), cv.theme().amber, Font::SmallSemi);
                }
            }
        } else {
            piece(L"Last indexed ", cv.theme().outline);
            piece(format_ago(sh_.index_state.last_index_unix), cv.theme().on_surface_variant, Font::SmallSemi);
        }
        if (sh_.db_count) {
            sep();
            FixedWString<40> files_txt;
            files_txt.assign(format_count(*sh_.db_count));
            files_txt.append(L" files");
            piece(files_txt, cv.theme().outline);
        }
        const auto enc = sh_.engine.encoder().state();
        if (enc == TextEncoder::State::Exporting) {
            sep();
            piece(L"Exporting text ONNX…", cv.theme().primary);
        } else if (enc == TextEncoder::State::Loading) {
            sep();
            piece(L"Loading model…", cv.theme().primary);
        }
        // A search fails with the model's own error while there is no model: show it once
        const std::wstring model_error = enc == TextEncoder::State::Failed ? sh_.engine.encoder().error() : std::wstring{};
        if (!model_error.empty()) {
            sep();
            piece(model_error, cv.theme().error);
        } else if (sh_.engine.db_paused_for_indexing() && !sh_.test_mode) {
            sep();
            piece(L"Database unavailable while indexing", cv.theme().amber);
        } else if (sh_.engine.db_loading() && !sh_.test_mode) {
            sep();
            piece(L"Database loading…", cv.theme().primary);
        } else if (!sh_.engine.db_ready() && !sh_.test_mode) {
            sep();
            piece(L"Database unavailable", cv.theme().error);
        }
        if (!error_.empty() && error_ != model_error) {
            sep();
            piece(error_, cv.theme().error);
        } else if (last_ms_ > 0 && expanded()) {
            sep();
            FixedWString<48> ms_txt = to_fixed_wstr(last_ms_, 0);
            ms_txt.append(L" ms");
            piece(ms_txt, cv.theme().outline);
        }
        sep();
        if (cached_model_utf8_ != sh_.settings.model_name) {
            cached_model_utf8_ = sh_.settings.model_name;
            cached_model_w_ = to_wide(cached_model_utf8_);
        }
        piece(cached_model_w_, cv.theme().outline);
        if (sh_.hotkey_unavailable()) {
            sep();
            FixedWString<128> hk_msg = format_hotkey(sh_.settings.hotkey_mods, sh_.settings.hotkey_vk);
            hk_msg.append(L" is taken by another app");
            piece(hk_msg, cv.theme().amber);
        }
        const float badge_rad = std::min(6.0f, cv.theme().button_radius * 0.6f);
        if (!sh_.notifications.empty()) {
            sep();
            const bool has_err = sh_.error_notification_count() > 0;
            const D2D1_COLOR_F nc = has_err ? cv.theme().error : cv.theme().amber;
            FixedWString<48> ntxt;
            ntxt.assign(format_count(sh_.notifications.size()));
            ntxt.append(sh_.notifications.size() == 1 ? L" alert" : L" alerts");
            const float nw = cv.measure(ntxt, Font::SmallSemi);
            const D2D1_RECT_F nr = rect(x - 4, y + 3, nw + 8, STATUS_H - 6);
            if (const float ha = hover_.alpha(IdStatusNotify); ha > 0.002f) {
                cv.fill_round(nr, badge_rad, with_alpha(nc, 0.14f * ha));
            }
            cv.text(ntxt, rect(x, y, nw + 2, STATUS_H), Font::SmallSemi, nc);
            cv.hit(nr, IdStatusNotify);
            x += nw;
        }

        float right_edge = pr.right - 18.0f;
        if (is_grid_zoomed()) {
            const D2D1_RECT_F zr = rect(right_edge - 24.0f, y + 3.0f, 24.0f, STATUS_H - 6.0f);
            const float ha = hover_.alpha(IdZoomReset);
            cv.fill_round(zr, badge_rad, lerp_color(with_alpha(cv.theme().primary, 0.14f), with_alpha(cv.theme().primary, 0.26f), ha));
            cv.icon(glyph::search, zr, lerp_color(cv.theme().primary, cv.theme().on_primary_container, ha * 0.35f), Font::IconSmall);
            cv.hit(zr, IdZoomReset);
            right_edge -= 32.0f;
        }
        if (expanded() && !items_.empty()) {
            FixedWString<80> right;
            if (selected_.size() > 1) {
                right.assign(format_count(selected_.size()));
                right.append(L" selected  •  ");
            }
            right.append(format_count(items_.size()));
            if (has_more_) right.push_back(L'+');
            right.append(L" results");
            cv.text(right, D2D1::RectF(x + 12, y, right_edge, y + STATUS_H), Font::Caption, cv.theme().outline, Align::Right);
        }
        if (expanded()) cv.hline(pr.left, pr.right, pr.top + HEADER_H + STATUS_H, with_alpha(cv.theme().outline_variant, 0.5f));
    }

    void draw_grid(Canvas& cv) {
        const Grid g = grid();
        cv.hit(g.view, IdGrid);
        cv.push_clip(g.view);

        if (items_.empty()) {
            const wchar_t* msg = (searching_ || debounce_pending_) ? L"" : (error_.empty() ? L"No matches" : L"");
            cv.text(msg, D2D1::RectF(g.view.left, g.view.top, g.view.right, g.view.top + 120), Font::Body, cv.theme().outline, Align::Center);
            if (searching_) draw_spinner(cv, {.x = (g.view.left + g.view.right) / 2, .y = g.view.top + 60});
        }

        const int first_row = std::max(0, static_cast<int>((scroll_ - PAD) / (g.card_h + GAP)));
        const int last_row = static_cast<int>((scroll_ + height(g.view)) / (g.card_h + GAP)) + 1;
        const UINT raw_px = static_cast<UINT>(std::max(g.card_w, g.thumb_h * 1.6f) * target_.scale());
        const UINT px = std::clamp(((raw_px + 63u) / 64u) * 64u, 64u, 2048u);
        thumb_px_ = px;
        if (cv.intersects_clip(g.view, 1.0f)) {
            const int first_vis = std::max(0, first_row * g.cols);
            const int last_vis = std::min<int>(static_cast<int>(items_.size()), (last_row + 1) * g.cols);
            for (int i = first_vis; i < last_vis; ++i) {
                if (cv.intersects_clip(card_rect(g, i), 2.0f)) draw_card(cv, g, i);
            }
            // Request visible thumbnails last so they're served first (LIFO); skip when drawing a dirty rect
            if (!cv.has_dirty_clip()) {
                for (int i = std::min<int>(static_cast<int>(items_.size()), (last_row + 2) * g.cols) - 1; i >= std::max(0, (first_row - 1) * g.cols); --i) {
                    UINT cached_px = 0;
                    if (!sh_.thumbs.get(items_[i].pt.id, &cached_px) || cached_px < px) {
                        sh_.thumbs.request(items_[i].pt.id, item_path(items_[i]), px);
                    }
                }
            }
        }

        if (has_more_ && !items_.empty()) {
            const float bottom = g.view.top + g.content_h - scroll_ - PAD - 20;
            if (loading_more_) draw_spinner(cv, {.x = (g.view.left + g.view.right) / 2, .y = bottom});
            // Infinite scroll: fetch the next page when nearing the end
            if (!loading_more_ && !searching_ && bottom < g.view.bottom + height(g.view)) {
                loading_more_ = true;
                PostMessageW(hwnd_, WM_APP_CALL, 1, 0);
            }
        }
        cv.pop_clip();

        // Overlay scrollbar
        const float ms = max_scroll();
        if (ms > 0) {
            const float track_h = height(g.view) - 8;
            const float thumb_h = std::max(28.0f, track_h * height(g.view) / g.content_h);
            const float ty = g.view.top + 4 + ((track_h - thumb_h) * (scroll_ / ms));
            const float ha = dragging_scroll_ ? 1.0f : hover_.alpha(IdScrollbar);
            const D2D1_RECT_F tr = D2D1::RectF(g.view.right - lerp(7.0f, 9.0f, ha), ty, g.view.right - 3, ty + thumb_h);
            cv.fill_round(tr, 3, with_alpha(cv.theme().on_surface_variant, lerp(0.28f, 0.55f, ha)));
            cv.hit(D2D1::RectF(g.view.right - 12, g.view.top, g.view.right, g.view.bottom), IdScrollbar);
            scroll_thumb_ = tr;
        }
    }

    void draw_spinner(Canvas& cv, D2D1_POINT_2F c) {
        const float t = static_cast<float>(GetTickCount64() % 1200) / 1200.0f;
        for (int i = 0; i < 8; ++i) {
            const float a = ((static_cast<float>(i) / 8.0f) + t) * 6.2831853f;
            const float alpha = 0.15f + (0.85f * (static_cast<float>(i) / 8.0f));
            cv.fill_circle({.x = c.x + (std::cos(a) * 9), .y = c.y + (std::sin(a) * 9)}, 2.0f, with_alpha(cv.theme().primary, alpha));
        }
        start_anim();
    }

    [[nodiscard]] float card_hover_alpha(int i) const noexcept {
        return card_hover_.alpha(IdCard, i);
    }

    void draw_card(Canvas& cv, const Grid& g, int i) {
        Item& it = items_[i];
        const D2D1_RECT_F r = card_rect(g, i);
        const float ha = card_hover_alpha(i);
        const bool sel = selected_.contains(it.pt.id);
        const bool focus = focus_ == i;

        const bool glass_ui = is_liquid_glass(cv.theme());
        const bool dark = cv.theme().dark_mode;
        const float crad = cv.theme().card_radius;
        const float trad = cv.theme().thumb_radius;
        if (glass_ui) {
            const D2D1_COLOR_F card_fill = with_alpha(lerp_color(cv.theme().surface_low, cv.theme().surface_high, ha),
                                                      dark ? lerp(0.28f, 0.40f, ha) : lerp(0.38f, 0.52f, ha));
            const D2D1_COLOR_F card_border = sel
                ? cv.theme().primary
                : (focus ? with_alpha(cv.theme().primary, 0.72f)
                         : with_alpha(hex(0xffffff), dark ? lerp(0.18f, 0.36f, ha) : lerp(0.55f, 0.85f, ha)));
            draw_glass_box(cv, r, crad, card_fill, card_border, sel ? 2.0f : 1.0f);
        } else {
            cv.fill_round(r, crad, with_alpha(lerp_color(cv.theme().surface_low, cv.theme().surface_high, ha), 0.92f));
            if (sel) {
                cv.stroke_round(r, crad, cv.theme().primary, 2.0f);
            } else if (focus) {
                cv.stroke_round(r, crad, with_alpha(cv.theme().primary, 0.6f), 1.0f);
            } else {
                cv.stroke_round(r, crad, with_alpha(cv.theme().outline_variant, lerp(0.45f, 0.9f, ha)));
            }
        }
        cv.hit(r, IdCard, i);

        // Thumbnail (cropped or letterboxed through a reusable bitmap brush so the corners stay rounded)
        const D2D1_RECT_F tr = cv.snap(rect(r.left + 6, r.top + 6, g.card_w - 12, g.thumb_h));
        if (ID2D1Bitmap1* bmp = sh_.thumbs.get(it.pt.id)) {
            const ThumbFit fit = parse_thumb_fit(sh_.settings.thumb_fit);
            if (fit == ThumbFit::Contain) {
                cv.fill_round(tr, trad, glass_ui ? with_alpha(cv.theme().surface_lowest, dark ? 0.24f : 0.35f) : cv.theme().surface_lowest);
            }
            cv.fill_bitmap_round(bmp, tr, trad, fit);
        } else {
            cv.fill_round(tr, trad, with_alpha(cv.theme().surface_highest, glass_ui ? (dark ? 0.24f : 0.36f) : 0.6f));
            cv.icon(it.pt.p.k == 1 ? glyph::video : glyph::photo, tr, with_alpha(cv.theme().outline, 0.6f), Font::IconLarge);
        }

        if (it.pt.p.k == 1) {
            const std::wstring_view dur = it.duration_w;
            const float dw = cv.measure(dur, Font::Caption);
            const D2D1_RECT_F b = rect(tr.right - dw - 34, tr.bottom - 30, dw + 26, 22);
            cv.fill_round(b, std::min(11.0f, offset_radius(cv.theme().button_radius, 1.0f)), hex(0x000000, 0.62f));
            cv.icon(glyph::play, rect(b.left + 6, b.top, 14, 22), hex(0xffffff), Font::IconSmall);
            cv.text(dur, rect(b.left + 21, b.top, dw + 2, 22), Font::Caption, hex(0xffffff));
        }
        if (ha > 0.002f) {
            const Canvas::ScopedOpacity op{cv, ha};
            const float ob_rad = std::min(14.0f, offset_radius(cv.theme().button_radius, 4.0f));
            const auto overlay_btn = [&](float x, wchar_t g, int id) {
                const D2D1_RECT_F sb = rect(x, tr.top + 8, 28, 28);
                const float ba = hover_.alpha(id, i);
                if (glass_ui) {
                    const D2D1_COLOR_F obg = lerp_color(hex(0x000000, 0.36f),
                                                        with_alpha(cv.theme().surface_high, dark ? 0.56f : 0.74f), ba);
                    const D2D1_COLOR_F obd = lerp_color(with_alpha(hex(0xffffff), 0.38f),
                                                        with_alpha(cv.theme().primary, 0.72f), ba);
                    draw_glass_box(cv, sb, ob_rad, obg, obd, 1.0f, dark ? 0.24f : 0.44f);
                    cv.icon(g, sb, lerp_color(hex(0xffffff), cv.theme().primary, ba), Font::IconSmall);
                } else {
                    cv.fill_round(sb, ob_rad, lerp_color(hex(0x000000, 0.55f), with_alpha(cv.theme().surface_high, 0.95f), ba));
                    cv.icon(g, sb, lerp_color(hex(0xffffff), cv.theme().primary, ba), Font::IconSmall);
                }
                cv.hit(sb, id, i);
            };
            overlay_btn(tr.right - 36, glyph::similar, IdSimilar);
            const wchar_t card_btn_glyph =
                (parse_thumb_dblclick(sh_.settings.thumb_dblclick) == ThumbDblClick::Preview) ? glyph::open : glyph::zoom;
            overlay_btn(tr.right - 70, card_btn_glyph, IdCardPreview);
        }

        float y = tr.bottom + 10.0f;
        const float lx = r.left + 12.0f;
        const float rw = g.card_w - 24.0f;
        const D2D1_RECT_F name_r = rect(lx, y, rw, 18.0f);
        cv.text(it.name, name_r, Font::BodySemi, cv.theme().on_surface);
        if (cv.measure(it.name, Font::BodySemi) > rw) cv.hit(name_r, IdCardName, i);
        y += 20.0f;
        const D2D1_RECT_F dir_r = rect(lx, y, rw, 16.0f);
        if (std::abs(it.dir_disp_w - rw) > 0.25f) {
            middle_truncate_dir_into(cv, it.dir, Font::MonoSmall, rw, it.dir_disp);
            it.dir_disp_w = rw;
        }
        cv.text(it.dir_disp, dir_r, Font::MonoSmall, cv.theme().outline);
        if (cv.measure(it.dir, Font::MonoSmall) > rw) cv.hit(dir_r, IdCardDir, i);
        y += 22.0f;

        // Tags (single line, +N overflow)
        float tx = lx;
        size_t shown = 0;
        for (const auto& t : it.tags_w) {
            const float w = cv.measure(t, Font::Caption) + 23.0f;
            if (tx + w > lx + rw - 34.0f && shown < it.tags_w.size()) break;
            draw_tag_chip(cv, tx, y, t, cv.tag_color(it.pt.p.t[shown]));
            cv.hit(rect(tx, y, w, 20.0f), IdTagChip, i);
            tx += w + 5.0f;
            ++shown;
        }
        if (shown < it.tags_w.size()) {
            std::array<wchar_t, 16> more_buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
            const int more_len = swprintf_s(more_buf.data(), more_buf.size(), L"+%zu", it.tags_w.size() - shown);
            const std::wstring_view more{more_buf.data(), more_len > 0 ? static_cast<size_t>(more_len) : 0u};
            const float w = cv.measure(more, Font::Caption) + 12.0f;
            const D2D1_RECT_F mr = rect(tx, y, w, 20.0f);
            cv.fill_round(mr, 10.0f, with_alpha(cv.theme().on_surface, 0.08f));
            cv.text(more, mr, Font::Caption, cv.theme().on_surface_variant, Align::Center);
            cv.hit(mr, IdCardMoreTags, i);
        }
        y += 28.0f;
        cv.hline(lx, lx + rw, y, with_alpha(cv.theme().outline_variant, 0.6f));
        y += 8.0f;
        cv.text(it.meta, rect(lx, y, rw * 0.62f, 18.0f), Font::MonoSmall, cv.theme().on_surface_variant);
        cv.text(it.date, rect(lx + (rw * 0.5f), y, rw * 0.5f, 18.0f), Font::Caption, cv.theme().outline, Align::Right);
    }

    // ------------------------------------------------------------------------------------------
    // Tag autocomplete dropdown
    // ------------------------------------------------------------------------------------------

    /// Current token if it is a tag filter: returns {prefix, start offset of value}.
    [[nodiscard]] std::optional<std::pair<std::wstring, size_t>> tag_token() const {
        const std::wstring& t = field_.text;
        size_t start = field_.caret;
        while (start > 0 && !iswspace(t[start - 1])) --start;
        const std::wstring_view tok = std::wstring_view{t}.substr(start, field_.caret - start);
        for (const std::wstring_view p : {L"-tag:", L"!tag:", L"tag:"}) {
            if (tok.size() >= p.size() && std::ranges::equal(tok.substr(0, p.size()), p, [](wchar_t a, wchar_t b) noexcept {
                    return std::towlower(a) == std::towlower(b);
                })) {
                return std::pair{lower(tok.substr(p.size())), start + p.size()};
            }
        }
        return std::nullopt;
    }

    void update_dropdown() {
        const auto tok = tag_token();
        if (!tok && !drop_from_button_) {
            if (drop_open_) {
                drop_open_ = false;
                apply_height();
            }
            return;
        }
        const std::wstring prefix = tok ? tok->first : L"";
        drop_items_.clear();
        for (const auto& tc : sh_.tags) {
            const std::wstring w = to_wide(tc.v);
            if (prefix.empty() || w.contains(prefix)) drop_items_.push_back(tc);
        }
        std::ranges::stable_sort(drop_items_, [&](const TagCount& a, const TagCount& b) {
            const bool pa = to_wide(a.v).starts_with(prefix);
            const bool pb = to_wide(b.v).starts_with(prefix);
            return pa > pb;
        });
        if (drop_items_.size() > 8) drop_items_.resize(8);
        drop_open_ = !drop_items_.empty() || drop_from_button_;
        if (drop_items_.empty()) {
            drop_sel_ = -1;
        } else if (drop_sel_ < 0 || std::cmp_greater_equal(drop_sel_, drop_items_.size())) {
            drop_sel_ = 0;
        }
        apply_height();
    }

    void accept_dropdown(int index) {
        if (index < 0 || std::cmp_greater_equal(index ,drop_items_.size())) return;
        const std::wstring tag = to_wide(drop_items_[index].v);
        const std::wstring value = tag.contains(L' ') ? ulp::str::concat_strings(L"\"", tag, L"\"") : tag;
        if (auto tok = tag_token()) {
            size_t end = field_.caret;
            while (end < field_.text.size() && !iswspace(field_.text[end])) ++end;
            field_.replace_range(tok->second, end - tok->second, ulp::str::concat_strings(value, L" "));
        } else {
            std::wstring t = trim(field_.text);
            if (!t.empty()) t.push_back(L' ');
            append_to(t, L"tag:", value, L" ");
            field_.replace_all(t);
        }
        drop_open_ = false;
        drop_from_button_ = false;
        field_.focused = true;
        run_search(0);
    }

    void draw_dropdown(Canvas& cv, D2D1_RECT_F pr) {
        const float x = pr.left + 44.0f;
        const float y = pr.top + HEADER_H - 4.0f;
        const float w = 300.0f;
        const float row = 32.0f;
        const float h = drop_items_.empty() ? 44.0f : (8.0f + (row * static_cast<float>(drop_items_.size())));
        const D2D1_RECT_F r = rect(x, y, w, h);
        const float pop_rad = offset_radius(cv.theme().card_radius, -2.0f);
        const float row_rad = offset_radius(cv.theme().button_radius, -2.0f);
        cv.shadow(r, pop_rad);
        if (!glass_active() || !glass_.draw_scene_glass(cv, target_, sh_.gfx, r, make_overlay_glass_style(cv.theme(), pop_rad, 1.8f))) {
            cv.fill_round(r, pop_rad, cv.theme().surface_high);
        }
        cv.stroke_round(r, pop_rad, with_alpha(cv.theme().outline_variant, glass_active() ? 0.55f : 1.0f));
        cv.hit(r, IdDrop);
        if (drop_items_.empty()) {
            cv.text(L"No tags yet", inset(r, 14, 0), Font::Small, cv.theme().outline);
            return;
        }
        for (size_t i = 0; i < drop_items_.size(); ++i) {
            const auto& t = drop_items_[i];
            const D2D1_RECT_F rr = rect(x + 4, y + 4 + (row * static_cast<float>(i)), w - 8, row);
            if (std::cmp_equal(i, drop_sel_)) cv.fill_round(rr, row_rad, with_alpha(cv.theme().primary, 0.14f));
            const D2D1_COLOR_F c = cv.tag_color(t.v);
            cv.fill_circle({.x=rr.left + 14, .y=(rr.top + rr.bottom) / 2}, 4, c);
            cv.text(to_wide(t.v), D2D1::RectF(rr.left + 28, rr.top, rr.right - 60, rr.bottom), Font::Body, cv.theme().on_surface);
            cv.text(format_count(t.c), D2D1::RectF(rr.right - 70, rr.top, rr.right - 12, rr.bottom), Font::Caption, cv.theme().outline, Align::Right);
            cv.hit(rr, IdDropItem, static_cast<int>(i));
        }
    }

    struct HelpRow {
        const wchar_t* syntax;
        const wchar_t* desc;
        const wchar_t* insert;
    };
    static constexpr std::array<HelpRow, 7> HELP_ROWS = {{
        {.syntax=L"free text", .desc=L"Semantic search by meaning (CLIP)", .insert=L""},
        {.syntax=L"name:cat", .desc=L"File name contains prefix tokens", .insert=L"name:"},
        {.syntax=L"tag:fav", .desc=L"Require tag (\"quotes\" for spaces)", .insert=L"tag:"},
        {.syntax=L"-tag:wip", .desc=L"Exclude tag (or !tag:wip)", .insert=L"-tag:"},
        {.syntax=L"dir:C:\\Photos", .desc=L"Search inside folder (or in:, bare path)", .insert=L"dir:"},
        {.syntax=L"similar:path", .desc=L"Visually similar to an indexed file", .insert=L"similar:"},
        {.syntax=L"type:image", .desc=L"Filter media type (image or video)", .insert=L"type:image "},
    },};

    void draw_help(Canvas& cv, D2D1_RECT_F pr) {
        const float x = std::clamp(help_btn_x_ + 36.0f - HELP_W, pr.left + 14.0f, std::max(pr.left + 14.0f, pr.right - HELP_W - 14.0f));
        const float y = pr.top + HEADER_H - 4.0f;
        const D2D1_RECT_F r = rect(x, y, HELP_W, HELP_H);
        const float pop_rad = offset_radius(cv.theme().card_radius, -2.0f);
        const float row_rad = offset_radius(cv.theme().button_radius, -2.0f);
        const float badge_rad = std::min(6.0f, cv.theme().button_radius * 0.6f);
        cv.shadow(r, pop_rad, 12, 0.35f);
        if (!glass_active() || !glass_.draw_scene_glass(cv, target_, sh_.gfx, r, make_overlay_glass_style(cv.theme(), pop_rad, 1.8f))) {
            cv.fill_round(r, pop_rad, cv.theme().surface_high);
        }
        cv.stroke_round(r, pop_rad, with_alpha(cv.theme().outline_variant, glass_active() ? 0.55f : 1.0f));
        cv.hit(rect(x, pr.top + HEADER_H - 18.0f, HELP_W, HELP_H + 14.0f), IdHelp);

        cv.text(L"SEARCH SYNTAX  ·  CLICK TO INSERT", rect(x + 14.0f, y + 8.0f, HELP_W - 28.0f, 22.0f), Font::Label, cv.theme().primary);
        float ry = y + 34.0f;
        for (int i = 0; std::cmp_less(i, std::size(HELP_ROWS)); ++i) {
            const D2D1_RECT_F rr = rect(x + 6.0f, ry, HELP_W - 12.0f, 28.0f);
            if (const float ha = hover_.alpha(IdHelpItem, i); ha > 0.002f && HELP_ROWS[i].insert[0] != 0) {
                cv.fill_round(rr, row_rad, with_alpha(cv.theme().primary, 0.12f * ha));
            }
            const D2D1_RECT_F badge = rect(rr.left + 6.0f, rr.top + 3.0f, 114.0f, 22.0f);
            cv.fill_round(badge, badge_rad, with_alpha(cv.theme().surface_highest, 0.9f));
            cv.text(HELP_ROWS[i].syntax, inset(badge, 8.0f, 0.0f), Font::MonoSmall, cv.theme().primary);
            cv.text(HELP_ROWS[i].desc, D2D1::RectF(rr.left + 130.0f, rr.top, rr.right - 8.0f, rr.bottom), Font::Small, cv.theme().on_surface_variant);
            if (HELP_ROWS[i].insert[0] != 0) cv.hit(rr, IdHelpItem, i);
            ry += 28.0f;
        }
    }

    void insert_help_syntax(int index) {
        if (index < 0 || std::cmp_greater_equal(index, std::size(HELP_ROWS))) return;
        const std::wstring_view ins = HELP_ROWS[index].insert;
        if (ins.empty()) return;
        std::wstring t = trim(field_.text);
        if (!t.empty()) t += L' ';
        t += ins;
        field_.replace_all(t);
        field_.focused = true;
        help_pinned_ = false;
        clear_hover();
        caret_reset();
        update_dropdown();
        if (ins.ends_with(L' ')) {
            run_search(0);
        } else {
            apply_height();
        }
        invalidate();
    }

    // ------------------------------------------------------------------------------------------
    // Context menu (actions + tag editor)
    // ------------------------------------------------------------------------------------------

    void open_menu(int index, D2D1_POINT_2F at) {
        menu_open_ = true;
        menu_index_ = index;
        menu_at_ = at;
        menu_field_ = TextField{};
        menu_field_.placeholder = L"Add tag";
        menu_field_.focused = true;
        field_.focused = false;
        drop_open_ = false;
        drop_from_button_ = false;
        apply_height();
        // Tags to offer: everything on the selection first, then the most used ones
        menu_tags_.clear();
        std::set<std::string> seen;
        for (const auto& it : items_) {
            if (!selected_.contains(it.pt.id)) continue;
            for (const auto& t : it.pt.p.t) {
                if (seen.insert(t).second) menu_tags_.push_back(t);
            }
        }
        for (const auto& t : sh_.tags) {
            if (menu_tags_.size() >= 10) break;
            if (seen.insert(t.v).second) menu_tags_.push_back(t.v);
        }
        caret_reset();
        invalidate();
    }

    void close_popups() noexcept {
        tooltip_.clear(hwnd_, TIMER_TOOLTIP);
        menu_open_ = false;
        drop_open_ = false;
        drop_from_button_ = false;
        help_pinned_ = false;
        field_.focused = true;
        apply_height();
    }

    /// 0 = none of the selection has it, 1 = some, 2 = all
    [[nodiscard]] int tag_state(const std::string& tag) const {
        size_t with = 0;
        size_t total = 0;
        for (const auto& it : items_) {
            if (!selected_.contains(it.pt.id)) continue;
            ++total;
            if (std::ranges::find(it.pt.p.t, tag) != it.pt.p.t.end()) ++with;
        }
        return with == 0 ? 0 : with == total ? 2 : 1;
    }

    void set_tag(const std::string& tag, bool on) {
        if (tag.empty()) return;
        struct Change {
            uint64_t id;
            std::string path;
            std::vector<std::string> tags;
        };
        std::vector<Change> changes;
        for (auto& it : items_) {
            if (!selected_.contains(it.pt.id)) continue;
            auto& t = it.pt.p.t;
            const auto pos = std::ranges::find(t, tag);
            if (on && pos == t.end()) {
                t.push_back(tag);
            } else if (!on && pos != t.end()) {
                t.erase(pos);
            } else {
                continue;
            }
            it.tags_w.clear();
            it.tags_w.reserve(t.size());
            for (const auto& s : t) it.tags_w.push_back(to_wide(s));
            changes.emplace_back(it.pt.id, it.pt.p.p, t);
        }
        if (changes.empty()) return;
        if (on && std::ranges::find(menu_tags_, tag) == menu_tags_.end()) menu_tags_.insert(menu_tags_.begin(), tag);
        sh_.engine.post([this, changes = std::move(changes)](Db& db) {
            const std::string coll = sh_.engine.collection();
            std::string first_err;
            size_t fail_cnt = 0;
            for (const auto& c : changes) {
                std::string err;
                if (!db.set_tags(coll, std::span{&c.id, 1}, c.tags, &err)) {
                    ++fail_cnt;
                    if (first_err.empty()) first_err = std::move(err);
                }
            }
            std::string flush_err;
            if (!db.flush(coll, &flush_err) && fail_cnt == 0) {
                fail_cnt = 1;
                first_err = std::move(flush_err);
            }
            std::string store_err;
            {
                TagStore store;
                if (!store.open(tags_db_path(), &store_err)) {
                    // keep store_err to notify below
                } else {
                    for (const auto& c : changes) {
                        std::string err;
                        if (!store.set_file_tags(c.id, c.path, c.tags, &err) && store_err.empty()) {
                            store_err = std::move(err);
                        }
                    }
                }
            }
            if (fail_cnt > 0 || !store_err.empty()) {
                sh_.call([this, fail_cnt, first_err, store_err] {
                    if (fail_cnt > 0) {
                        sh_.notify("error", "Tags",
                            ulp::str::concat_strings("Failed to update tags on ", to_fixed_str(fail_cnt), fail_cnt == 1 ? " item" : " items"),
                            first_err, "tags-save");
                    }
                    if (!store_err.empty()) {
                        sh_.notify("warning", "Tags", "Failed to save tags to tags.db", store_err, "tags-backup");
                    }
                });
            }
            sh_.refresh_tags();
        });
        invalidate();
    }

    void open_item_file(const std::wstring& path) {
        std::string err;
        if (!open_file(path, &err)) {
            sh_.notify("error", "Open File", "Failed to open file", ulp::str::concat_strings(err, ": ", to_utf8(path)), "open-file");
        }
    }

    void reveal_item_file(const std::wstring& path) {
        std::string err;
        if (!reveal_in_explorer(path, &err)) {
            sh_.notify("error", "Explorer", "Failed to reveal file in Explorer", ulp::str::concat_strings(err, ": ", to_utf8(path)), "reveal-file");
        }
    }

    void copy_item_text(const std::wstring& text) {
        if (!set_clipboard_text(hwnd_, text)) {
            sh_.notify("error", "Clipboard", "Failed to copy path to clipboard", win32_error_utf8(), "clipboard");
        }
    }

    [[nodiscard]] D2D1_RECT_F menu_rect() const noexcept {
        const D2D1_RECT_F pr = pill_bounds(target_.size());
        const float h = (static_cast<float>(ActCount) * 36.0f) + 16.0f + 30.0f + (static_cast<float>(std::min<size_t>(menu_tags_.size(), 10)) * 32.0f) + 48.0f;
        const float x = std::min(menu_at_.x, pr.right - MENU_W - 8);
        float y = menu_at_.y;
        if (y + h > pr.bottom - 8) y = std::max(pr.top + 8.0f, pr.bottom - 8 - h);
        return rect(std::max(pr.left + 8.0f, x), y, MENU_W, h);
    }

    void draw_menu(Canvas& cv) {
        const D2D1_RECT_F r = menu_rect();
        const float menu_rad = cv.theme().card_radius;
        const float row_rad = offset_radius(cv.theme().button_radius, -2.0f);
        const float box_rad = std::min(4.0f, cv.theme().button_radius * 0.4f);
        cv.shadow(r, menu_rad, 14, 0.4f);
        if (!glass_active() || !glass_.draw_scene_glass(cv, target_, sh_.gfx, r, make_overlay_glass_style(cv.theme(), menu_rad, 2.0f))) {
            cv.fill_round(r, menu_rad, cv.theme().surface_high);
        }
        cv.stroke_round(r, menu_rad, with_alpha(cv.theme().outline_variant, glass_active() ? 0.55f : 1.0f));
        cv.hit(r, IdMenu);

        struct Act {
            wchar_t g;
            const wchar_t* label;
            const wchar_t* key;
        };
        static constexpr std::array<Act, ActCount> acts = {{
            {.g = glyph::open, .label = L"Open", .key = L"Enter"},
            {.g = glyph::zoom, .label = L"Preview", .key = L"Space"},
            {.g = glyph::folder, .label = L"Show in Explorer", .key = L"Ctrl+E"},
            {.g = glyph::similar, .label = L"Find similar", .key = L"Ctrl+S"},
            {.g = glyph::copy, .label = L"Copy media", .key = L"Ctrl+C"},
            {.g = glyph::link, .label = L"Copy path", .key = L"Ctrl+Shift+C"},
        },};
        float y = r.top + 6;
        for (int i = 0; i < ActCount; ++i) {
            const D2D1_RECT_F rr = rect(r.left + 6, y, MENU_W - 12, 36);
            if (const float ha = hover_.alpha(IdMenuItem, i); ha > 0.002f) cv.fill_round(rr, row_rad, with_alpha(cv.theme().on_surface, 0.08f * ha));
            cv.icon(acts[i].g, rect(rr.left + 8, rr.top, 20, 36), cv.theme().on_surface_variant);
            cv.text(acts[i].label, D2D1::RectF(rr.left + 38, rr.top, rr.right - 86, rr.bottom), Font::Body, cv.theme().on_surface);
            cv.text(acts[i].key, D2D1::RectF(rr.right - 96, rr.top, rr.right - 10, rr.bottom), Font::Caption, cv.theme().outline, Align::Right);
            cv.hit(rr, IdMenuItem, i);
            y += 36;
        }
        y += 4;
        cv.hline(r.left + 12, r.right - 12, y, cv.theme().outline_variant);
        y += 6;
        FixedWString<48> label{L"TAGS"};
        if (selected_.size() > 1) {
            label.append(L"  ·  ");
            label.append(to_fixed_wstr(selected_.size()));
            label.append(L" ITEMS");
        }
        cv.text(label, rect(r.left + 16, y, MENU_W - 32, 24), Font::Label, cv.theme().primary);
        y += 26;
        for (size_t i = 0; i < menu_tags_.size() && i < 10; ++i) {
            const auto& t = menu_tags_[i];
            const D2D1_RECT_F rr = rect(r.left + 6, y, MENU_W - 12, 32);
            if (const float ha = hover_.alpha(IdMenuTag, static_cast<int>(i)); ha > 0.002f) cv.fill_round(rr, row_rad, with_alpha(cv.theme().on_surface, 0.08f * ha));
            const int st = tag_state(t);
            const D2D1_RECT_F box = rect(rr.left + 10, rr.top + 8, 16, 16);
            if (st == 2) {
                cv.fill_round(box, box_rad, cv.theme().primary);
                cv.icon(glyph::check, box, cv.theme().on_primary, Font::IconSmall);
            } else {
                cv.stroke_round(box, box_rad, st == 1 ? cv.theme().primary : cv.theme().outline, 1.5f);
                if (st == 1) cv.fill(rect(box.left + 4, box.top + 7, 8, 2), cv.theme().primary);
            }
            const D2D1_COLOR_F c = cv.tag_color(t);
            cv.fill_circle({.x = rr.left + 40, .y = (rr.top + rr.bottom) / 2}, 3.5f, c);
            cv.text(to_wide(t), D2D1::RectF(rr.left + 50, rr.top, rr.right - 10, rr.bottom), Font::Body, cv.theme().on_surface);
            cv.hit(rr, IdMenuTag, static_cast<int>(i));
            y += 32;
        }
        const D2D1_RECT_F fr = rect(r.left + 10, y + 6, MENU_W - 20, 34);
        cv.fill_round(fr, row_rad, cv.theme().surface_highest);
        if (menu_field_.focused) cv.stroke_round(fr, row_rad, cv.theme().primary, 1.5f);
        cv.icon(glyph::add, rect(fr.left + 4, fr.top, 24, 34), cv.theme().outline, Font::IconSmall);
        menu_field_.draw(cv, D2D1::RectF(fr.left + 30, fr.top, fr.right - 10, fr.bottom), Font::Body, cv.theme().on_surface, cv.theme().outline);
        cv.hit(fr, IdMenuField);
    }

    void menu_action(int a) {
        std::vector<const Item*> sel;
        for (const auto& it : items_) {
            if (selected_.contains(it.pt.id)) sel.push_back(&it);
        }
        const bool have_index = menu_index_ >= 0 && std::cmp_less(menu_index_, items_.size());
        if (sel.empty() && have_index) sel.push_back(&items_[menu_index_]);
        if (sel.empty()) return;
        // Single-item actions follow the item the menu was opened on (or the keyboard focus)
        const Item& primary = have_index && selected_.contains(items_[menu_index_].pt.id) ? items_[menu_index_] : *sel.front();
        close_popups();
        switch (a) {
            case ActOpen:
                for (size_t i = 0; i < sel.size() && i < 8; ++i) open_item_file(item_path(*sel[i]));
                break;
            case ActPreview: open_preview(static_cast<int>(&primary - items_.data())); return;
            case ActReveal: reveal_item_file(item_path(primary)); break;
            case ActSimilar: {
                const Item copy = primary;
                if (preview_ >= 0) close_preview(false);
                find_similar(copy);
                break;
            }
            case ActCopy: copy_media(sel); break;
            case ActCopyPath: {
                std::wstring all;
                for (const auto* it : sel) {
                    if (!all.empty()) all += L"\r\n";
                    all += item_path(*it);
                }
                copy_item_text(all);
                break;
            }
            default:
                break;
        }
        invalidate();
    }

    void find_similar(const Item& it) {
        field_.replace_all(ulp::str::concat_strings(L"similar:", item_path(it)));
        field_.focused = true;
        run_search(0);
    }

    /// Copies the files themselves; a single still image also goes on as pixels so editors can paste it.
    /// Only the decode runs on a worker; the newest copy wins and reaches the clipboard from the UI thread.
    void copy_media(std::span<const Item* const> sel) {
        if (sel.empty()) return;
        std::vector<std::wstring> paths;
        paths.reserve(sel.size());
        for (const auto* it : sel) paths.push_back(item_path(*it));
        const Item& one = *sel.front();
        const bool decode = sel.size() == 1 && one.pt.p.k == 0 && !svg::is_svg(paths[0]) && static_cast<uint64_t>(one.pt.p.w) * one.pt.p.h <= COPY_MAX_PIXELS;

        if (pending_clipboard_) {
            KillTimer(hwnd_, TIMER_COPY);
            pending_clipboard_.reset();
            end_copy();
        }
        for (auto& [id, worker] : copy_workers_) worker.request_stop();
        const uint64_t id = ++copy_generation_;
        copy_decoding_ = decode;
        ++sh_.background_jobs;
        copy_workers_[id] = std::jthread([this, id, decode, paths = std::move(paths)](const std::stop_token& stop) mutable {
            const ulp::win::COMWrapper<false> com{static_cast<DWORD>(COINIT_MULTITHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE)};
            const auto payload = std::make_shared<ClipboardPayload>();
            if (!stop.stop_requested()) {
                std::optional<Raster> image;
                std::optional<std::wstring> png;
                if (decode) {
                    image = decode_image(paths[0], 0, false);
                    if (image && static_cast<uint64_t>(image->w) * image->h > COPY_MAX_PIXELS) {
                        image.reset();
                    } else if (!image) {
                        std::string p_utf8 = to_utf8(paths[0]);
                        sh_.call([this, p_utf8 = std::move(p_utf8)] {
                            sh_.notify("warning", "Clipboard", "Copied file link only (image decode failed)",
                                ulp::str::concat_strings(p_utf8, " exceeded pixel limits or could not be decoded for bitmap paste."), "clipboard-decode");
                        });
                    }
                    if (lower(fs::path{paths[0]}.extension().wstring()) == L".png") png = paths[0];
                }
                *payload = build_clipboard_payload(std::move(paths), image ? &*image : nullptr, png ? &*png : nullptr);
            }
            sh_.call([this, id, payload] {
                copy_workers_.erase(id);
                if (id != copy_generation_) {
                    end_copy();
                    return;
                }
                copy_decoding_ = false;
                pending_clipboard_ = clipboard_data_object(*payload);
                clipboard_attempts_ = 0;
                set_clipboard();
            });
        });
    }

    /// The clipboard can be held open by another process for a moment: try again shortly instead of waiting here.
    void set_clipboard() {
        KillTimer(hwnd_, TIMER_COPY);
        const HRESULT hr = pending_clipboard_ ? OleSetClipboard(pending_clipboard_.get()) : E_FAIL;
        if (hr == CLIPBRD_E_CANT_OPEN && ++clipboard_attempts_ < CLIPBOARD_ATTEMPTS) {
            SetTimer(hwnd_, TIMER_COPY, 30, nullptr);
            return;
        }
        // Render now: this process exits when idle and the clipboard must outlive it
        if (SUCCEEDED(hr)) {
            OleFlushClipboard();
        } else {
            sh_.notify("error", "Clipboard", "Failed to copy media to clipboard",
                win32_error_utf8(static_cast<DWORD>(hr)), "clipboard");
        }
        pending_clipboard_.reset();
        end_copy();
    }

    void end_copy() {
        --sh_.background_jobs;
        if (sh_.visibility_changed) sh_.visibility_changed();
    }

    // ------------------------------------------------------------------------------------------
    // Full-size viewer
    // ------------------------------------------------------------------------------------------

    static constexpr float PV_TOP = HEADER_H;
    static constexpr float PV_BOTTOM = 44.0f;
    static constexpr float PV_CONTROLS_H = 42.0f;

    [[nodiscard]] bool preview_valid() const noexcept { return preview_ >= 0 && std::cmp_less(preview_, items_.size()); }

    [[nodiscard]] bool preview_has_controls() const noexcept {
        if (!preview_valid()) return false;
        const Item& it = items_[preview_];
        if (it.pt.p.k == 1) return !video_.failed();
        if (is_gif_path(it.name)) return gif_.has_animation() || (gif_.active() && !gif_.settled_static());
        return false;
    }

    [[nodiscard]] static D2D1_RECT_F preview_media_area(D2D1_RECT_F bounds, bool has_controls) noexcept {
        const float bottom = std::max(bounds.top + PV_TOP + 48.0f, bounds.bottom - PV_BOTTOM - (has_controls ? PV_CONTROLS_H : 0.0f));
        return D2D1::RectF(bounds.left + 16.0f, bounds.top + PV_TOP, std::max(bounds.left + 32.0f, bounds.right - 16.0f), bottom);
    }

    [[nodiscard]] D2D1_RECT_F preview_dest_dims(uint32_t src_w, uint32_t src_h, uint32_t meta_w, uint32_t meta_h, D2D1_RECT_F area) const noexcept {
        const FitBox b = compute_preview_box(src_w, src_h, meta_w, meta_h, target_.scale(),
                                             FitBox{.left = area.left, .top = area.top, .width = width(area), .height = height(area)});
        return rect(b.left, b.top, b.width, b.height);
    }

    [[nodiscard]] D2D1_RECT_F preview_dest(const Item& it, ID2D1Bitmap* bmp, D2D1_RECT_F area) const noexcept {
        const D2D1_SIZE_U ps = bmp->GetPixelSize();
        return preview_dest_dims(ps.width, ps.height, it.pt.p.w, it.pt.p.h, area);
    }

    bool prepare_preview_media() {
        if (!preview_valid()) return true;
        const Item& it = items_[preview_];
        if (it.pt.p.k == 1) {
            if (video_.needs_recover() && sh_.gfx.d3d()) {
                video_.recover_if_needed(sh_.gfx.d3d(), sh_.gfx.epoch());
                full_dirty_ = true;
                sync_preview_timers();
            }
            if (video_.active() && !video_.failed()) {
                const uint32_t nw = video_.native_width();
                const uint32_t nh = video_.native_height();
                if (nw > 0 && nh > 0) {
                    const D2D1_RECT_F area = preview_media_area(pill_bounds(target_.size()), preview_has_controls());
                    const D2D1_RECT_F dest = preview_dest_dims(nw, nh, nw, nh, area);
                    const auto px_w = static_cast<uint32_t>(std::max(1.0f, std::round(width(dest) * target_.scale())));
                    const auto px_h = static_cast<uint32_t>(std::max(1.0f, std::round(height(dest) * target_.scale())));
                    const bool had_frame = video_.has_frame();
                    const bool resized = video_.set_output_size(px_w, px_h);
                    if (resized || video_.is_seeking()) {
                        const MediaTickStatus st = video_.tick(sh_.gfx.d3d(), target_.dc());
                        if (st == MediaTickStatus::DeviceLost) {
                            recover();
                            return false;
                        }
                        if (st == MediaTickStatus::FrameReady) {
                            last_scrubber_redraw_ = std::chrono::steady_clock::now();
                            if (!had_frame || !queue_preview_media_dirty()) full_dirty_ = true;
                            sync_preview_timers();
                        }
                    }
                }
            }
        } else if (gif_.active() && gif_.has_frame() && gif_.sync_bitmap(target_.dc()) == MediaTickStatus::DeviceLost) {
            recover();
            return false;
        }
        return true;
    }

    void set_video_timer(bool active) noexcept {
        if (!hwnd_) {
            video_timer_active_ = false;
            return;
        }
        if (active) {
            if (!video_timer_active_) {
                SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
                video_timer_active_ = true;
            }
        } else if (video_timer_active_) {
            KillTimer(hwnd_, TIMER_VIDEO);
            video_timer_active_ = false;
        }
    }

    void sync_preview_timers() noexcept {
        if (!hwnd_) return;
        if (!visible() || !preview_valid()) {
            KillTimer(hwnd_, TIMER_GIF);
            set_video_timer(false);
            return;
        }
        const Item& it = items_[preview_];
        if (it.pt.p.k == 1) {
            KillTimer(hwnd_, TIMER_GIF);
            set_video_timer(video_.active() && !video_.failed() && video_.needs_timer());
        } else {
            set_video_timer(false);
            if (gif_.has_animation() && gif_.playing()) {
                SetTimer(hwnd_, TIMER_GIF, std::clamp(gif_.current_delay_ms(), 10u, 60000u), nullptr);
            } else {
                KillTimer(hwnd_, TIMER_GIF);
            }
        }
    }

    void invalidate_preview_controls() {
        if (!visible()) return;
        if (pv_ctrl_rect_.right <= pv_ctrl_rect_.left || pv_ctrl_rect_.bottom <= pv_ctrl_rect_.top) {
            invalidate();
            return;
        }
        pending_dirty_.push_back(D2D1::RectF(pv_ctrl_rect_.left - 4.0f, pv_ctrl_rect_.top - 4.0f,
                                             pv_ctrl_rect_.right + 4.0f, pv_ctrl_rect_.bottom + 4.0f));
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool queue_preview_media_dirty() {
        if (pv_media_rect_.right <= pv_media_rect_.left || pv_media_rect_.bottom <= pv_media_rect_.top) {
            return false;
        }
        pending_dirty_.push_back(D2D1::RectF(pv_media_rect_.left - 2.0f, pv_media_rect_.top - 2.0f,
                                             pv_media_rect_.right + 2.0f, pv_media_rect_.bottom + 2.0f));
        if (pv_ctrl_rect_.right > pv_ctrl_rect_.left && pv_ctrl_rect_.bottom > pv_ctrl_rect_.top) {
            pending_dirty_.push_back(D2D1::RectF(pv_ctrl_rect_.left - 4.0f, pv_ctrl_rect_.top - 4.0f,
                                                 pv_ctrl_rect_.right + 4.0f, pv_ctrl_rect_.bottom + 4.0f));
        }
        return true;
    }

    void invalidate_preview_media() {
        if (!visible()) return;
        if (!queue_preview_media_dirty()) {
            invalidate();
            return;
        }
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void on_gif_worker() {
        if (!preview_valid() || items_[preview_].pt.p.k == 1) return;
        const bool had_anim = gif_.has_animation();
        const bool had_frame = gif_.has_frame();
        const bool had_static_err = gif_.static_fallback_error();
        if (gif_.process_worker_signal()) {
            if (!had_static_err && gif_.static_fallback_error()) {
                sh_.notify("warning", "Preview", "Animated GIF playback unavailable",
                    ulp::str::concat_strings("Could not decode animated GIF frames; displaying static thumbnail instead (",
                        to_utf8(item_path(items_[preview_])), ")."),
                    "preview-gif");
            }
            sync_preview_timers();
            if (had_anim != gif_.has_animation() || had_frame != gif_.has_frame() || gif_.settled_static()) {
                invalidate();
            } else {
                invalidate_preview_media();
            }
        }
    }

    void on_gif_timer() {
        KillTimer(hwnd_, TIMER_GIF);
        if (!visible() || !preview_valid() || items_[preview_].pt.p.k == 1 || !gif_.has_animation() || !gif_.playing()) return;
        const auto [advanced, wait_ms] = gif_.tick(std::chrono::steady_clock::now());
        if (wait_ms > 0) SetTimer(hwnd_, TIMER_GIF, wait_ms, nullptr);
        if (advanced) invalidate_preview_media();
    }

    void on_video_event() {
        const bool had_frame = video_.has_frame();
        const bool had_failed = video_.failed();
        const bool changed = video_.process_events();
        if (!had_failed && video_.failed() && preview_valid() && items_[preview_].pt.p.k == 1) {
            sh_.notify("warning", "Preview", "Inline video playback failed",
                ulp::str::concat_strings("Could not play video inline (", to_utf8(item_path(items_[preview_])),
                    "). Click Play to open in your default media player."),
                "preview-video");
        }
        if (!preview_valid() || items_[preview_].pt.p.k != 1) {
            sync_preview_timers();
            return;
        }
        if (target_.ready()) {
            const MediaTickStatus st = video_.tick(sh_.gfx.d3d(), target_.dc());
            if (st == MediaTickStatus::DeviceLost) {
                recover();
                return;
            }
            sync_preview_timers();
            if (had_frame != video_.has_frame() || had_failed != video_.failed()) {
                invalidate();
            } else if (st == MediaTickStatus::FrameReady || changed) {
                last_scrubber_redraw_ = std::chrono::steady_clock::now();
                invalidate_preview_media();
            }
        } else {
            sync_preview_timers();
            if (changed) invalidate();
        }
    }

    void on_video_timer() {
        if (!visible() || !preview_valid() || items_[preview_].pt.p.k != 1 || !video_.active() || !video_.needs_timer()) {
            set_video_timer(false);
            return;
        }
        const bool had_frame = video_.has_frame();
        const MediaTickStatus st = target_.ready() ? video_.tick(sh_.gfx.d3d(), target_.dc()) : MediaTickStatus::NoChange;
        if (st == MediaTickStatus::DeviceLost) {
            recover();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (st == MediaTickStatus::FrameReady) {
            last_scrubber_redraw_ = now;
            if (!had_frame) {
                invalidate();
            } else {
                invalidate_preview_media();
            }
        } else if (video_.is_playing() &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scrubber_redraw_).count() >= 200) {
            last_scrubber_redraw_ = now;
            invalidate_preview_controls();
        }
        if (!video_.needs_timer()) set_video_timer(false);
    }

    void open_preview(int index) {
        if (index < 0 || std::cmp_greater_equal(index, items_.size())) return;
        close_popups();
        clear_hover();
        preview_wheel_ = 0;
        preview_initial_index_ = index;
        preview_initial_scroll_ = scroll_;
        preview_go(index);
    }

    void reset_preview_interaction_state() noexcept {
        gif_scrub_resume_ = false;
        const bool was_seek = std::exchange(dragging_seek_, false);
        const bool was_vol = std::exchange(dragging_volume_, false);
        if ((was_seek || was_vol) && GetCapture() == hwnd_) ReleaseCapture();
        pv_media_rect_ = {};
        pv_ctrl_rect_ = {};
        pv_seek_rect_ = {};
        pv_vol_rect_ = {};
    }

    void close_preview(bool restore_focus = true) {
        if (preview_ < 0) return;
        const int last = preview_;
        preview_ = -1;
        reset_preview_interaction_state();
        KillTimer(hwnd_, TIMER_GIF);
        set_video_timer(false);
        gif_.close();
        video_.close();
        sh_.previews.clear();
        clear_hover();
        if (restore_focus && std::cmp_less(last, items_.size())) {
            focus_ = last;
            field_.focused = false;
            if (last != preview_initial_index_) {
                scroll_to(last, false);
            } else {
                scroll_ = target_scroll_ = preview_initial_scroll_;
            }
        }
        invalidate();
    }

    void preview_go(int index) {
        if (items_.empty()) {
            close_preview(false);
            return;
        }
        index = std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
        reset_preview_interaction_state();
        preview_ = index;
        focus_ = index;
        selected_ = {items_[index].pt.id};
        field_.focused = false;
        // Browsing towards the end pulls in the next page, as scrolling the grid would
        if (index >= static_cast<int>(items_.size()) - 3 && has_more_ && !loading_more_ && !searching_) run_search(pagination_.next_offset());
        request_preview();
        invalidate();
    }

    void preview_step(int d) {
        if (!preview_valid()) return;
        const int n = std::clamp(preview_ + d, 0, static_cast<int>(items_.size()) - 1);
        if (n != preview_) preview_go(n);
    }

    /// Decodes are bounded by the monitor, so the image is sharp even when the window goes fullscreen.
    [[nodiscard]] UINT preview_px() const noexcept {
        MONITORINFO mi{.cbSize = sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        return static_cast<UINT>(std::max({mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, 256L}));
    }

    void request_preview() {
        if (!preview_valid()) return;
        const UINT px = preview_px();
        const auto want = [&](int k) {
            if (k >= 0 && std::cmp_less(k, items_.size())) {
                const Item& target = items_[static_cast<size_t>(k)];
                if (target.pt.p.k == 1) return;
                sh_.previews.request(target.pt.id, item_path(target), px);
            }
        };
        // Newest requests are served first: neighbours, then the shown one
        want(preview_ + 1);
        want(preview_ - 1);
        want(preview_);
        const Item& it = items_[preview_];
        const std::wstring path = item_path(it);
        if (thumb_px_ && !sh_.thumbs.get(it.pt.id)) sh_.thumbs.request(it.pt.id, path, thumb_px_);

        if (it.pt.p.k == 1) {
            gif_.close();
            apply_video_audio(false);
            video_.open(path, sh_.gfx.d3d(), sh_.gfx.epoch());
        } else if (is_gif_path(it.name)) {
            video_.close();
            gif_.open(it.pt.id, path, px);
        } else {
            video_.close();
            gif_.close();
        }
        sync_preview_timers();
    }

    void draw_preview_slider(Canvas& cv, D2D1_RECT_F bar, float ratio, int id,
                             bool active_drag, D2D1_COLOR_F fill_col, float pad_l, float pad_r) {
        const float cy = (bar.top + bar.bottom) * 0.5f;
        const float thumb_x = bar.left + ((bar.right - bar.left) * std::clamp(ratio, 0.0f, 1.0f));
        const float ha = hover_.alpha(id);
        if (is_liquid_glass(cv.theme())) {
            const bool dark = cv.theme().dark_mode;
            const D2D1_RECT_F track = D2D1::RectF(bar.left, cy - 2.5f, bar.right, cy + 2.5f);
            draw_glass_box(cv, track, 2.5f,
                           with_alpha(hex(0xffffff), dark ? 0.16f : 0.28f),
                           with_alpha(hex(0xffffff), dark ? 0.22f : 0.48f), 1.0f, dark ? 0.12f : 0.28f);
            if (thumb_x > bar.left + 0.5f) {
                const D2D1_RECT_F filled = D2D1::RectF(bar.left, cy - 2.5f, thumb_x, cy + 2.5f);
                cv.fill_round(filled, 2.5f, with_alpha(fill_col, 0.90f));
                if (width(filled) > 4.0f) {
                    cv.hline(filled.left + 2.0f, filled.right - 2.0f, filled.top + 0.5f, with_alpha(hex(0xffffff), 0.40f));
                }
            }
            const bool expanded_knob = active_drag || ha > 0.05f;
            const float kw = expanded_knob ? 20.0f : 15.0f;
            const float kh = expanded_knob ? 13.0f : 11.0f;
            const float krad = kh * 0.5f;
            const D2D1_RECT_F knob = rect(thumb_x - (kw * 0.5f), cy - (kh * 0.5f), kw, kh);
            cv.shadow(knob, krad, 5.0f, 0.34f);
            const D2D1_COLOR_F knob_tint = active_drag
                ? with_alpha(cv.theme().primary, dark ? 0.36f : 0.30f)
                : with_alpha(hex(0xffffff), dark ? lerp(0.38f, 0.52f, ha) : lerp(0.68f, 0.82f, ha));
            const GlassStyle gs = make_control_glass_style(cv.theme(), krad, knob_tint, 1.0f, 0.4f, 5.5f, 1.16f);
            if (!glass_.draw_scene_glass(cv, target_, sh_.gfx, knob, gs)) {
                draw_glass_box(cv, knob, krad, knob_tint, with_alpha(hex(0xffffff), 0.78f));
            } else {
                cv.stroke_round(knob, krad, with_alpha(hex(0x000000), dark ? 0.30f : 0.16f), 0.8f);
            }
        } else {
            cv.fill_round(D2D1::RectF(bar.left, cy - 2.0f, bar.right, cy + 2.0f), 2.0f, cv.theme().surface_highest);
            if (thumb_x > bar.left + 0.5f) {
                cv.fill_round(D2D1::RectF(bar.left, cy - 2.0f, thumb_x, cy + 2.0f), 2.0f, fill_col);
            }
            const float rad = (active_drag || ha > 0.05f) ? 5.5f : 4.2f;
            cv.fill_circle({.x = thumb_x, .y = cy}, rad, lerp_color(cv.theme().on_surface, cv.theme().primary, active_drag ? 1.0f : ha * 0.5f));
        }
        cv.hit(D2D1::RectF(bar.left - pad_l, bar.top, bar.right + pad_r, bar.bottom), id);
    }

    void draw_preview_controls(Canvas& cv, D2D1_RECT_F bounds, D2D1_RECT_F area, D2D1_RECT_F dest,
                               const Item& it, bool is_video, bool is_gif) {
        const bool is_video_ctrl = is_video && video_.active() && !video_.failed();
        const bool is_gif_ctrl = is_gif && gif_.has_animation();
        if (!is_video_ctrl && !is_gif_ctrl) {
            pv_ctrl_rect_ = {};
            pv_seek_rect_ = {};
            pv_vol_rect_ = {};
            return;
        }

        const float max_bar_w = std::max(280.0f, width(bounds) - 140.0f);
        const float min_ctrl_w = is_video_ctrl ? 380.0f : 320.0f;
        const float cw = std::clamp(std::max(min_ctrl_w, width(dest)), 280.0f, max_bar_w);
        const float cx = bounds.left + ((width(bounds) - cw) * 0.5f);
        const D2D1_RECT_F ctrl = rect(cx, area.bottom + 4.0f, cw, 34.0f);
        pv_ctrl_rect_ = ctrl;

        const float ctrl_rad = std::min(17.0f, offset_radius(cv.theme().button_radius, 7.0f));
        if (is_liquid_glass(cv.theme())) {
            const bool dark = cv.theme().dark_mode;
            const GlassStyle bar_gs = make_control_glass_style(
                cv.theme(), ctrl_rad, with_alpha(cv.theme().surface_high, dark ? 0.30f : 0.42f), 1.0f, 1.6f, 12.0f, 1.06f);
            if (!glass_.draw_scene_glass(cv, target_, sh_.gfx, ctrl, bar_gs)) {
                draw_glass_box(cv, ctrl, ctrl_rad,
                               with_alpha(cv.theme().surface_high, dark ? 0.36f : 0.50f),
                               with_alpha(hex(0xffffff), dark ? 0.24f : 0.64f));
            } else {
                cv.stroke_round(ctrl, ctrl_rad, with_alpha(hex(0x000000), dark ? 0.28f : 0.10f), 1.0f);
            }
        } else {
            cv.fill_round(ctrl, ctrl_rad, with_alpha(cv.theme().surface_low, 0.92f));
            cv.stroke_round(ctrl, ctrl_rad, cv.theme().outline_variant, 1.0f);
        }
        cv.hit(ctrl, IdPvBar);

        float lx = ctrl.left + 5.0f;
        float rx = ctrl.right - 5.0f;
        const bool playing = is_video_ctrl
            ? (video_.is_scrubbing() ? video_.want_play() : video_.is_playing())
            : (dragging_seek_ ? gif_scrub_resume_ : gif_.playing());
        const D2D1_RECT_F play_r = rect(lx, ctrl.top + 3.0f, 28.0f, 28.0f);
        icon_button(cv, play_r, playing ? glyph::pause : glyph::play, IdPvPlay);
        lx += 32.0f;

        float ratio = 0.0f;
        if (is_video_ctrl) {
            double dur_s = video_.duration();
            if (dur_s <= 0.0 && it.pt.p.d > 0) dur_s = static_cast<double>(it.pt.p.d) / 1000.0;
            const double cur_s = dragging_seek_ ? static_cast<double>(seek_drag_ratio_) * dur_s : video_.current_time();
            ratio = dragging_seek_ ? seek_drag_ratio_
                                   : (dur_s > 0.0 ? static_cast<float>(std::clamp(cur_s / dur_s, 0.0, 1.0)) : 0.0f);

            const auto cur_str = format_duration_ms(static_cast<uint64_t>(std::max(0.0, cur_s * 1000.0)));
            const FixedWString<32> dur_str = dur_s > 0.0
                ? format_duration_ms(static_cast<uint64_t>(dur_s * 1000.0))
                : FixedWString<32>{L"--:--"};
            const float cur_w = std::max(34.0f, cv.measure(cur_str, Font::MonoSmall) + 6.0f);
            const float dur_w = std::max(34.0f, cv.measure(dur_str, Font::MonoSmall) + 6.0f);

            constexpr float vol_track_w = 50.0f;
            const bool muted_vis = effective_video_muted();
            const float avail_seek = (rx - lx) - cur_w - 4.0f - dur_w - 4.0f - 32.0f;
            if (avail_seek >= (vol_track_w + 15.0f) + 76.0f) {
                const float vx1 = rx - 7.0f;
                const float vx0 = vx1 - vol_track_w;
                pv_vol_rect_ = D2D1::RectF(vx0, ctrl.top, vx1, ctrl.bottom);
                draw_preview_slider(cv, pv_vol_rect_, displayed_video_volume(), IdPvVolume,
                                    dragging_volume_, muted_vis ? cv.theme().outline : cv.theme().primary, 4.0f, 6.0f);
                rx = vx0 - 8.0f;
            } else {
                pv_vol_rect_ = {};
            }

            const D2D1_RECT_F mute_r = rect(rx - 28.0f, ctrl.top + 3.0f, 28.0f, 28.0f);
            icon_button(cv, mute_r, muted_vis ? glyph::mute : glyph::volume, IdPvMute, muted_vis);
            rx -= 32.0f;

            cv.text(cur_str, rect(lx, ctrl.top, cur_w, height(ctrl)), Font::MonoSmall, cv.theme().on_surface_variant, Align::Center);
            lx += cur_w + 4.0f;
            cv.text(dur_str, rect(rx - dur_w, ctrl.top, dur_w, height(ctrl)), Font::MonoSmall, cv.theme().outline, Align::Center);
            rx -= dur_w + 4.0f;
        } else {
            pv_vol_rect_ = {};
            const D2D1_RECT_F badge_r = rect(lx + 2.0f, ctrl.top + 8.0f, 30.0f, 18.0f);
            cv.fill_round(badge_r, 9.0f, with_alpha(cv.theme().surface_highest, 0.95f));
            cv.text(L"GIF", badge_r, Font::Label, cv.theme().primary, Align::Center);
            lx += 36.0f;

            const size_t total = gif_.frame_count();
            const size_t cur = dragging_seek_
                ? static_cast<size_t>(std::lround(seek_drag_ratio_ * static_cast<float>(total > 0 ? total - 1 : 0)))
                : gif_.current_frame();
            ratio = dragging_seek_ ? seek_drag_ratio_
                                   : (total > 1 ? static_cast<float>(cur) / static_cast<float>(total - 1) : 0.0f);

            FixedWString<48> fstr = to_fixed_wstr<48>(cur + 1);
            fstr.push_back(L'/');
            fstr.append(to_fixed_wstr<48>(total));
            const float fw = std::max(38.0f, cv.measure(fstr, Font::MonoSmall) + 8.0f);
            cv.text(fstr, rect(rx - fw, ctrl.top, fw, height(ctrl)), Font::MonoSmall, cv.theme().outline, Align::Center);
            rx -= fw + 4.0f;
        }

        const float sx0 = lx + 6.0f;
        const float sx1 = rx - 6.0f;
        if (sx1 > sx0 + 16.0f) {
            pv_seek_rect_ = D2D1::RectF(sx0, ctrl.top, sx1, ctrl.bottom);
            draw_preview_slider(cv, pv_seek_rect_, ratio, IdPvSeek, dragging_seek_, cv.theme().primary, 6.0f, 6.0f);
        } else {
            pv_seek_rect_ = {};
        }
    }

    void draw_preview(Canvas& cv, D2D1_RECT_F bounds, bool custom_frame, float outer_r) {
        if (!preview_valid()) {
            preview_ = -1;
            if (glass_active()) glass_.end_scene(target_);
            return;
        }
        const Item& it = items_[preview_];
        const int count = static_cast<int>(items_.size());
        const bool glass_ui = is_liquid_glass(cv.theme());
        const bool dark = cv.theme().dark_mode;
        const bool scene_clipped = custom_frame && cv.push_round_clip(bounds, outer_r);
        if (!glass_ui) {
            if (custom_frame) {
                cv.fill_round(bounds, outer_r, with_alpha(cv.theme().surface_lowest, 0.94f));
            } else {
                cv.fill(bounds, with_alpha(cv.theme().surface_lowest, 0.94f));
            }
        }
        // Backdrop below the title bar closes the viewer; the bar itself still drags the window
        cv.hit(D2D1::RectF(bounds.left, bounds.top + PV_TOP, bounds.right, bounds.bottom), IdPreview);

        const bool is_video = (it.pt.p.k == 1);
        const bool is_gif_ready = !is_video && gif_.has_animation() && gif_.has_frame() && gif_.bitmap();
        const D2D1_RECT_F area = preview_media_area(bounds, preview_has_controls());
        const D2D1_INTERPOLATION_MODE img_interp = in_sizemove_
            ? D2D1_INTERPOLATION_MODE_LINEAR
            : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;

        D2D1_RECT_F dest = inset(area, width(area) * 0.3f, height(area) * 0.3f);
        if (is_video && video_.has_frame() && video_.bitmap()) {
            dest = preview_dest_dims(video_.native_width(), video_.native_height(), video_.native_width(), video_.native_height(), area);
            cv.shadow(dest, 4, 14, 0.35f);
            const D2D1_RECT_F src = D2D1::RectF(0.0f, 0.0f, static_cast<float>(video_.frame_width()), static_cast<float>(video_.frame_height()));
            cv.bitmap_region(video_.bitmap(), dest, src, D2D1_INTERPOLATION_MODE_LINEAR);
            cv.hit(dest, IdPvImage);
        } else if (is_video && video_.audio_only() && !video_.failed()) {
            ID2D1Bitmap1* poster = sh_.thumbs.get(it.pt.id);
            if (poster) {
                dest = preview_dest(it, poster, area);
                cv.shadow(dest, 4, 14, 0.35f);
                cv.bitmap(poster, dest, img_interp);
                cv.hit(dest, IdPvImage);
            } else {
                cv.icon(glyph::video, rect(area.left, ((area.top + area.bottom) / 2) - 20, width(area), 40), cv.theme().on_surface_variant, Font::IconLarge);
            }
        } else if (is_gif_ready) {
            ID2D1Bitmap1* gbmp = gif_.bitmap();
            const D2D1_SIZE_U gps = gbmp->GetPixelSize();
            const uint32_t mw = gif_.native_width() > 0 ? gif_.native_width() : it.pt.p.w;
            const uint32_t mh = gif_.native_height() > 0 ? gif_.native_height() : it.pt.p.h;
            dest = preview_dest_dims(gps.width, gps.height, mw, mh, area);
            cv.shadow(dest, 4, 14, 0.35f);
            cv.bitmap(gbmp, dest, img_interp);
            cv.hit(dest, IdPvImage);
        } else {
            ID2D1Bitmap1* full = sh_.previews.get(it.pt.id);
            ID2D1Bitmap1* bmp = full ? full : sh_.thumbs.get(it.pt.id);
            const bool loading = is_video ? video_.is_loading() : (!full && !sh_.previews.failed(it.pt.id));
            if (bmp) {
                dest = preview_dest(it, bmp, area);
                cv.shadow(dest, 4, 14, 0.35f);
                cv.bitmap(bmp, dest, img_interp);
                cv.hit(dest, IdPvImage);
                if (loading) draw_spinner(cv, {.x = dest.right - 20, .y = dest.bottom - 20});
            } else if (loading) {
                draw_spinner(cv, {.x = (area.left + area.right) / 2, .y = (area.top + area.bottom) / 2});
            } else {
                cv.icon(is_video ? glyph::video : glyph::photo, rect(area.left, ((area.top + area.bottom) / 2) - 40, width(area), 40), cv.theme().outline, Font::IconLarge);
                cv.text(L"No preview available", rect(area.left, (area.top + area.bottom) / 2, width(area), 24), Font::Body, cv.theme().outline, Align::Center);
            }
            if (is_video && video_.failed()) {
                const D2D1_POINT_2F c{.x = (dest.left + dest.right) / 2, .y = (dest.top + dest.bottom) / 2};
                const float ha = hover_.alpha(IdPvPlay);
                cv.fill_circle(c, 30, lerp_color(hex(0x000000, 0.55f), with_alpha(cv.theme().surface_highest, 0.92f), ha));
                cv.icon(glyph::play, rect(c.x - 28, c.y - 30, 60, 60), lerp_color(hex(0xffffff), cv.theme().primary, ha), Font::IconLarge);
                cv.hit(rect(c.x - 30, c.y - 30, 60, 60), IdPvPlay);
                const wchar_t* msg = L"Video playback failed — click Play to open externally";
                const float mw = cv.measure(msg, Font::Caption) + 20.0f;
                const D2D1_RECT_F badge = rect(c.x - (mw * 0.5f), c.y + 38.0f, mw, 24.0f);
                cv.fill_round(badge, std::min(12.0f, offset_radius(cv.theme().button_radius, 2.0f)), hex(0x000000, 0.72f));
                cv.text(msg, badge, Font::Caption, cv.theme().amber, Align::Center);
            }
        }
        pv_media_rect_ = dest;
        if (custom_frame) cv.pop_round_clip(scene_clipped);
        if (glass_active()) glass_.end_scene(target_);

        const bool overlay_clipped = custom_frame && cv.push_round_clip(bounds, outer_r);

        draw_preview_controls(cv, bounds, area, dest, it, is_video, is_gif_ready);

        // Previous / next
        const D2D1_RECT_F nav_area = preview_media_area(bounds, false);
        const auto arrow = [&](bool next) {
            const bool can = next ? preview_ + 1 < count : preview_ > 0;
            if (!can) return;
            const float cx = next ? nav_area.right - 28 : nav_area.left + 28;
            const float cy = (nav_area.top + nav_area.bottom) / 2;
            const int id = next ? IdPvNext : IdPvPrev;
            const float ha = hover_.alpha(id);
            if (is_liquid_glass(cv.theme())) {
                const bool dark = cv.theme().dark_mode;
                const D2D1_RECT_F ar = rect(cx - 22.0f, cy - 22.0f, 44.0f, 44.0f);
                cv.shadow(ar, 22.0f, 8.0f, 0.28f);
                const GlassStyle ags = make_control_glass_style(
                    cv.theme(), 22.0f,
                    with_alpha(cv.theme().surface_high, dark ? lerp(0.28f, 0.46f, ha) : lerp(0.42f, 0.64f, ha)),
                    1.0f, 1.4f, 11.0f, 1.12f);
                if (!glass_.draw_scene_glass(cv, target_, sh_.gfx, ar, ags)) {
                    cv.fill_circle({.x = cx, .y = cy}, 22,
                                   with_alpha(cv.theme().surface_high, dark ? lerp(0.32f, 0.52f, ha) : lerp(0.46f, 0.68f, ha)));
                    cv.stroke_circle({.x = cx, .y = cy}, 22,
                                     with_alpha(hex(0xffffff), dark ? lerp(0.22f, 0.42f, ha) : lerp(0.55f, 0.85f, ha)), 1.0f);
                } else {
                    cv.stroke_circle({.x = cx, .y = cy}, 22.5f, with_alpha(hex(0x000000), dark ? 0.26f : 0.12f), 1.0f);
                }
                cv.icon(next ? glyph::chevron_right : glyph::chevron_left, ar,
                        lerp_color(with_alpha(cv.theme().on_surface, 0.85f), cv.theme().on_surface, ha));
            } else {
                cv.fill_circle({.x = cx, .y = cy}, 22, lerp_color(hex(0x000000, 0.45f), with_alpha(cv.theme().surface_highest, 0.96f), ha));
                cv.icon(next ? glyph::chevron_right : glyph::chevron_left, rect(cx - 22, cy - 22, 44, 44), lerp_color(hex(0xffffff, 0.9f), cv.theme().on_surface, ha));
            }
            cv.hit(rect(cx - 28, cy - 44, 56, 88), id);
        };
        arrow(false);
        arrow(true);

        // Title bar: name and folder, position, actions
        float x = bounds.right - 12;
        const float cy = bounds.top + (PV_TOP / 2);
        const auto btn = [&](wchar_t g, int id) {
            x -= 36;
            icon_button(cv, rect(x, cy - 18, 36, 36), g, id);
        };
        btn(glyph::close, IdPvClose);
        x -= 8;
        cv.vline(x, cy - 12, cy + 12, cv.theme().outline_variant);
        x -= 4;
        btn(glyph::copy, IdPvCopy);
        btn(glyph::folder, IdPvReveal);
        btn(glyph::open, IdPvOpen);
        FixedWString<64> pos;
        pos.assign(format_count(static_cast<uint64_t>(preview_) + 1));
        pos.append(L" / ");
        pos.append(format_count(items_.size()));
        if (has_more_) pos.push_back(L'+');
        const float pw = cv.measure(pos, Font::SmallSemi) + 24;
        x -= pw + 6;
        const D2D1_RECT_F pr = rect(x, cy - 13, pw, 26);
        const float pos_rad = std::min(13.0f, offset_radius(cv.theme().button_radius, 3.0f));
        if (is_liquid_glass(cv.theme())) {
            const GlassStyle pgs = make_control_glass_style(
                cv.theme(), pos_rad, with_alpha(cv.theme().surface_high, dark ? 0.28f : 0.40f), 1.0f, 1.2f, 8.0f, 1.08f);
            if (!glass_.draw_scene_glass(cv, target_, sh_.gfx, pr, pgs)) {
                draw_glass_box(cv, pr, pos_rad,
                               with_alpha(cv.theme().surface_high, dark ? 0.30f : 0.44f),
                               with_alpha(hex(0xffffff), dark ? 0.20f : 0.58f));
            }
        } else {
            cv.fill_round(pr, pos_rad, with_alpha(cv.theme().surface_highest, 0.85f));
        }
        cv.text(pos, pr, Font::SmallSemi, cv.theme().on_surface_variant, Align::Center);
        const float path_w = std::max(40.0f, x - bounds.left - 34.0f);
        if (pv_dir_disp_id_ != it.pt.id || std::abs(pv_dir_disp_w_ - path_w) > 0.25f) {
            middle_truncate_dir_into(cv, it.dir, Font::MonoSmall, path_w, pv_dir_disp_);
            pv_dir_disp_w_ = path_w;
            pv_dir_disp_id_ = it.pt.id;
        }
        cv.text(it.name, D2D1::RectF(bounds.left + 18, bounds.top + 9, x - 16, bounds.top + 29), Font::BodySemi, cv.theme().on_surface);
        cv.text(pv_dir_disp_, D2D1::RectF(bounds.left + 18, bounds.top + 29, x - 16, bounds.top + 46), Font::MonoSmall, cv.theme().outline);
        if (cv.measure(it.name, Font::BodySemi) > path_w || cv.measure(it.dir, Font::MonoSmall) > path_w) {
            cv.hit(D2D1::RectF(bounds.left + 18, bounds.top + 9, x - 16, bounds.top + 46), IdPvPath, preview_);
        }

        // Footer: size, date, duration
        FixedWString<128> meta;
        if (!it.duration_w.empty()) {
            meta.assign(it.duration_w);
            meta.append(L"  •  ");
        }
        meta.append(it.meta);
        if (!it.date.empty()) {
            meta.append(L"  •  ");
            meta.append(it.date);
        }
        cv.text(meta, D2D1::RectF(bounds.left + 18, bounds.bottom - PV_BOTTOM, bounds.right - 18, bounds.bottom), Font::Caption, cv.theme().outline, Align::Center);
        if (custom_frame) cv.pop_round_clip(overlay_clipped);
    }

    bool preview_seek_to_x(float x, bool force = false) noexcept {
        if (!preview_valid() || pv_seek_rect_.right <= pv_seek_rect_.left) return false;
        const float ratio = std::clamp((x - pv_seek_rect_.left) / width(pv_seek_rect_), 0.0f, 1.0f);
        if (!force && ratio == seek_drag_ratio_) return false;
        seek_drag_ratio_ = ratio;
        if (items_[preview_].pt.p.k == 1) {
            const double dur = video_.duration();
            if (dur > 0.0) {
                video_.seek(static_cast<double>(ratio) * dur);
                sync_preview_timers();
            }
        } else if (gif_.has_animation()) {
            gif_.seek_fraction(ratio);
            sync_preview_timers();
        }
        return true;
    }

    void begin_seek_drag(float x) {
        if (!preview_valid()) return;
        dragging_seek_ = true;
        if (items_[preview_].pt.p.k == 1 && !video_.failed()) {
            video_.begin_scrub();
        } else if (gif_.has_animation()) {
            gif_scrub_resume_ = gif_.playing();
            gif_.set_playing(false);
        }
        SetCapture(hwnd_);
        preview_seek_to_x(x, true);
        invalidate_preview_media();
    }

    void end_seek_drag() {
        if (!dragging_seek_) return;
        dragging_seek_ = false;
        const bool resume_gif = std::exchange(gif_scrub_resume_, false);
        if (preview_valid()) {
            if (items_[preview_].pt.p.k == 1 && !video_.failed()) {
                video_.end_scrub();
                sync_preview_timers();
            } else if (gif_.has_animation()) {
                gif_.set_playing(resume_gif);
                sync_preview_timers();
            }
        }
        invalidate_preview_controls();
    }

    [[nodiscard]] double normalized_video_volume() const noexcept {
        return std::isfinite(sh_.settings.video_volume) ? std::clamp(sh_.settings.video_volume, 0.0, 1.0) : 1.0;
    }

    [[nodiscard]] bool effective_video_muted() const noexcept {
        return sh_.test_mode || sh_.settings.video_muted || normalized_video_volume() <= 0.0;
    }

    [[nodiscard]] float displayed_video_volume() const noexcept {
        return effective_video_muted() ? 0.0f : static_cast<float>(normalized_video_volume());
    }

    void apply_video_audio(bool persist) {
        const double vol = normalized_video_volume();
        sh_.settings.video_volume = vol;
        video_.set_volume(vol);
        video_.set_muted(sh_.test_mode || sh_.settings.video_muted);
        if (persist && !sh_.test_mode) sh_.save();
    }

    void toggle_video_mute() {
        if (!preview_valid() || items_[preview_].pt.p.k != 1 || video_.failed()) return;
        if (sh_.settings.video_muted || normalized_video_volume() <= 0.0) {
            sh_.settings.video_muted = false;
            if (normalized_video_volume() <= 0.0) sh_.settings.video_volume = 0.5;
        } else {
            sh_.settings.video_muted = true;
        }
        apply_video_audio(true);
        invalidate_preview_controls();
    }

    void set_video_volume(double vol, bool persist) {
        const double clamped = std::clamp(std::isfinite(vol) ? vol : 1.0, 0.0, 1.0);
        const double snapped = clamped <= 0.005 ? 0.0 : clamped;
        sh_.settings.video_volume = snapped;
        sh_.settings.video_muted = (snapped <= 0.0);
        apply_video_audio(persist);
    }

    void nudge_video_volume(double delta) {
        if (!preview_valid() || items_[preview_].pt.p.k != 1 || video_.failed()) return;
        const double base = effective_video_muted() ? 0.0 : normalized_video_volume();
        set_video_volume(base + delta, true);
        invalidate_preview_controls();
    }

    bool apply_volume_from_x(float x, bool force = false) {
        if (!preview_valid() || pv_vol_rect_.right <= pv_vol_rect_.left) return false;
        const float ratio = std::clamp((x - pv_vol_rect_.left) / width(pv_vol_rect_), 0.0f, 1.0f);
        if (!force && std::abs(ratio - displayed_video_volume()) < 1e-4f) return false;
        set_video_volume(static_cast<double>(ratio), false);
        return true;
    }

    void begin_volume_drag(float x) {
        if (!preview_valid() || items_[preview_].pt.p.k != 1 || video_.failed()) return;
        dragging_volume_ = true;
        SetCapture(hwnd_);
        apply_volume_from_x(x, true);
        invalidate_preview_controls();
    }

    void end_volume_drag() {
        if (!dragging_volume_) return;
        dragging_volume_ = false;
        apply_video_audio(true);
        invalidate_preview_controls();
    }

    void preview_toggle_play() {
        if (!preview_valid()) return;
        if (items_[preview_].pt.p.k == 1 && !video_.failed()) {
            video_.toggle_play();
            sync_preview_timers();
            invalidate_preview_controls();
        } else if (gif_.has_animation()) {
            if (dragging_seek_) {
                gif_scrub_resume_ = !gif_scrub_resume_;
            } else {
                gif_.toggle_play();
            }
            sync_preview_timers();
            invalidate_preview_controls();
        }
    }

    void preview_relative_seek(double delta_s, int gif_dir) {
        if (!preview_valid()) return;
        const Item& it = items_[preview_];
        if (it.pt.p.k == 1 && !video_.failed()) {
            const double dur = video_.duration();
            if (dur > 0.0) {
                const double base = dragging_seek_ ? static_cast<double>(seek_drag_ratio_) * dur : video_.current_time();
                const double target = std::clamp(base + delta_s, 0.0, dur);
                if (dragging_seek_) seek_drag_ratio_ = static_cast<float>(target / dur);
                video_.seek(target);
                sync_preview_timers();
            }
            invalidate_preview_media();
        } else if (gif_.has_animation()) {
            gif_.step_frame(gif_dir);
            if (dragging_seek_) {
                const size_t total = gif_.frame_count();
                seek_drag_ratio_ = total > 1
                    ? static_cast<float>(gif_.current_frame()) / static_cast<float>(total - 1)
                    : 0.0f;
            }
            sync_preview_timers();
            invalidate_preview_media();
        }
    }

    void preview_click(int id, D2D1_POINT_2F p, bool dbl) {
        const std::wstring path = item_path(items_[preview_]);
        switch (id) {
            case IdPvClose: close_preview(); return;
            case IdPvPrev: preview_step(-1); return;
            case IdPvNext: preview_step(1); return;
            case IdPvOpen: open_item_file(path); return;
            case IdPvPlay:
                if (items_[preview_].pt.p.k == 1 && video_.failed()) {
                    open_item_file(path);
                } else {
                    preview_toggle_play();
                }
                return;
            case IdPvMute:
                toggle_video_mute();
                return;
            case IdPvVolume:
                begin_volume_drag(p.x);
                return;
            case IdPvSeek:
                begin_seek_drag(p.x);
                return;
            case IdPvBar: return;
            case IdPvReveal: reveal_item_file(path); return;
            case IdPvCopy: copy_media(std::array<const Item*, 1>{&items_[preview_]}); return;
            case IdPvImage:
                if (dbl) {
                    open_item_file(path);
                } else {
                    begin_drag_detect(preview_);
                }
                return;
            case IdPreview:
                if (!dbl) {
                    close_preview();
                    // The second half of a double-click must not land on the card underneath
                    swallow_dbl_ = true;
                }
                return;
            default: return;
        }
    }

    bool preview_key(UINT vk) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const Item& it = items_[preview_];
        switch (vk) {
            case VK_LEFT:
            case VK_UP:
            case VK_PRIOR: preview_step(-1); return true;
            case VK_RIGHT:
            case VK_DOWN:
            case VK_NEXT: preview_step(1); return true;
            case VK_HOME: preview_go(0); return true;
            case VK_END: preview_go(static_cast<int>(items_.size()) - 1); return true;
            case VK_SPACE:
                swallow_space_ = true;
                close_preview();
                return true;
            case VK_RETURN: open_item_file(item_path(it)); return true;
            case 'K':
            case 'P':
            case VK_MEDIA_PLAY_PAUSE:
                preview_toggle_play();
                return true;
            case 'M':
                toggle_video_mute();
                return true;
            case VK_OEM_PLUS:
            case VK_ADD:
                if (!ctrl) nudge_video_volume(0.05);
                return true;
            case VK_OEM_MINUS:
            case VK_SUBTRACT:
                if (!ctrl) nudge_video_volume(-0.05);
                return true;
            case 'J':
            case VK_OEM_COMMA:
                preview_relative_seek(vk == 'J' ? -5.0 : -0.05, -1);
                return true;
            case 'L':
            case VK_OEM_PERIOD:
                preview_relative_seek(vk == 'L' ? 5.0 : 0.05, 1);
                return true;
            default: break;
        }
        if (ctrl && vk == 'C') {
            if (shift) {
                copy_item_text(item_path(it));
            } else {
                copy_media(std::array<const Item*, 1>{&it});
            }
        } else if (ctrl && vk == 'E') {
            reveal_item_file(item_path(it));
        } else if (ctrl && vk == 'S') {
            const Item copy = it;
            close_preview(false);
            find_similar(copy);
        }
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Dragging media out to other applications
    // ------------------------------------------------------------------------------------------

    void begin_drag_detect(int index) noexcept {
        drag_pending_ = true;
        drag_index_ = index;
        GetCursorPos(&drag_origin_);
        SetCapture(hwnd_);
    }

    /// Mouse released without dragging: a click on one of several selected cards narrows to it now.
    void end_drag_detect() {
        if (!drag_pending_) return;
        drag_pending_ = false;
        if (deferred_select_) {
            selected_ = {*deferred_select_};
            deferred_select_.reset();
            invalidate();
        }
    }

    void start_drag() {
        drag_pending_ = false;
        deferred_select_.reset();
        ReleaseCapture();
        if (drag_index_ < 0 || std::cmp_greater_equal(drag_index_, items_.size())) return;
        const uint64_t id = items_[drag_index_].pt.id;
        if (!selected_.contains(id)) selected_ = {id};
        std::vector<std::wstring> paths;
        for (const auto& it : items_) {
            if (selected_.contains(it.pt.id)) paths.push_back(item_path(it));
        }
        clear_hover();
        clear_tooltip_now();
        render();
        dragging_out_ = true;
        // Not topmost while the target works, so its dialogs (e.g. file conflict prompts) are not hidden behind this window
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        std::string drag_err;
        auto async_drop = drag_media(hwnd_, paths, &drag_err);
        dragging_out_ = false;
        if (!drag_err.empty()) {
            sh_.notify("error", "Drag & Drop", "Failed to start file drag operation", drag_err, "drag-drop");
        }
        if (async_drop) {
            async_drops_.push_back(std::move(async_drop));
            ++sh_.background_jobs;
            SetTimer(hwnd_, TIMER_DROP, 250, nullptr);
        } else {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (sh_.settings.hide_on_focus_loss && !fullscreen_ && !sh_.test_mode && GetForegroundWindow() != hwnd_) hide();
        invalidate();
    }

    // ------------------------------------------------------------------------------------------
    // Input
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] D2D1_POINT_2F to_dip(LPARAM lp) const noexcept {
        const float s = target_.scale();
        return {.x = GET_X_LPARAM(lp) / s, .y = GET_Y_LPARAM(lp) / s};
    }

    void caret_reset() noexcept {
        field_.caret_on = true;
        menu_field_.caret_on = true;
        SetTimer(hwnd_, TIMER_CARET, GetCaretBlinkTime(), nullptr);
    }

    void on_mouse_move(D2D1_POINT_2F p) {
        if (fading_out_) return;
        if (!tracking_) {
            TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd_, .dwHoverTime = 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        if (drag_pending_) {
            if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                end_drag_detect();
                ReleaseCapture();
            } else {
                POINT c;
                GetCursorPos(&c);
                const UINT dpi = GetDpiForWindow(hwnd_);
                if (std::abs(c.x - drag_origin_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
                    std::abs(c.y - drag_origin_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi)) {
                    start_drag();
                    return;
                }
            }
        }
        if (dragging_seek_) {
            if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                end_seek_drag();
                ReleaseCapture();
            } else {
                if (preview_seek_to_x(p.x)) {
                    invalidate_preview_media();
                }
                return;
            }
        }
        if (dragging_volume_) {
            if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                end_volume_drag();
                ReleaseCapture();
            } else {
                if (apply_volume_from_x(p.x)) {
                    invalidate_preview_controls();
                }
                return;
            }
        }
        if (dragging_scroll_) {
            if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                dragging_scroll_ = false;
                ReleaseCapture();
            } else {
                const Grid g = grid();
                const float track = height(g.view) - 8 - height(scroll_thumb_);
                if (track > 0) {
                    scroll_ = target_scroll_ = std::clamp(drag_scroll_start_ + ((p.y - drag_y_) / track * max_scroll()), 0.0f, max_scroll());
                }
                invalidate();
                return;
            }
        }
        if (selecting_text_) {
            if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                selecting_text_ = false;
                ReleaseCapture();
            } else {
                Canvas cv{target_, sh_.fonts, hits_, sh_.theme};
                field_.click(cv, p.x, true);
                invalidate();
                return;
            }
        }
        const Hit* h = hit_test(hits_, p);
        const int id = h ? h->id : IdNone;
        const int idx = h ? h->index : 0;
        if (id != raw_hover_id_ || idx != raw_hover_index_) {
            const bool was_help = show_help();
            raw_hover_id_ = id;
            raw_hover_index_ = idx;
            const bool animate = visible() && !sh_.test_mode;
            const auto [vid, vidx] = visual_hover(id, idx);
            const auto [cid, cidx] = card_hover_target(id, idx);
            if (vid != hover_.id || vidx != hover_.index) {
                queue_hover_dirty(hover_.id, hover_.index);
                queue_hover_dirty(hover_.prev_id, hover_.prev_index);
                queue_hover_dirty(vid, vidx);
            }
            if (cid != card_hover_.id || cidx != card_hover_.index) {
                queue_hover_dirty(card_hover_.id, card_hover_.index);
                queue_hover_dirty(card_hover_.prev_id, card_hover_.prev_index);
                queue_hover_dirty(cid, cidx);
            }
            const bool item_changed = hover_.set(vid, vidx, animate);
            const bool card_changed = card_hover_.set(cid, cidx, animate);
            const bool vis_changed = item_changed || card_changed;
            std::wstring tt;
            std::wstring ts;
            const bool can_tip = !menu_open_ && !drop_open_ && !drag_pending_ && !dragging_out_ && tooltip_for(id, idx, tt, ts);
            tooltip_.on_hover_changed(hwnd_, TIMER_TOOLTIP, h, can_tip, animate);
            const bool tip_fading = animate && tooltip_.alpha.active();
            if (show_help() != was_help) {
                full_dirty_ = true;
                apply_height();
            }
            if ((vis_changed && animate) || tip_fading) {
                start_anim();
            } else if (vis_changed) {
                invalidate();
            }
        }
    }

    /// Hit regions are recorded while drawing, so a pending repaint is done before a mouse position is resolved.
    void flush() {
        if (dirty_) render();
    }

    void on_lbutton_down(D2D1_POINT_2F p, bool dbl) {
        flush();
        clear_tooltip_now();
        selecting_text_ = false;
        dragging_scroll_ = false;
        drag_pending_ = false;
        deferred_select_.reset();
        SetFocus(hwnd_);
        const Hit* h = hit_test(hits_, p);
        const int id = h ? h->id : IdNone;
        const int idx = h ? h->index : 0;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (dbl && swallow_dbl_) {
            swallow_dbl_ = false;
            return;
        }
        swallow_dbl_ = false;

        if (menu_open_ && id != IdMenu && id != IdMenuItem && id != IdMenuTag && id != IdMenuField) {
            close_popups();
            invalidate();
            return;
        }
        if (preview_valid() && !menu_open_) {
            preview_click(id, p, dbl);
            if (id != IdPvBar && id != IdPvPlay && id != IdPvMute && id != IdPvVolume && id != IdPvSeek) invalidate();
            return;
        }
        if (drop_open_ && id != IdDrop && id != IdDropItem && id != IdTagBtn && id != IdField) {
            drop_open_ = false;
            drop_from_button_ = false;
            apply_height();
        }
        if (help_pinned_ && id != IdHelp && id != IdHelpItem && id != IdHelpBtn) {
            help_pinned_ = false;
            apply_height();
        }
        switch (id) {
            case IdField: {
                field_.focused = true;
                Canvas cv{target_, sh_.fonts, hits_, sh_.theme};
                field_.click(cv, p.x, shift);
                if (dbl) field_.select_word_at_caret();
                selecting_text_ = !dbl;
                SetCapture(hwnd_);
                caret_reset();
                update_dropdown();
                break;
            }
            case IdChip:
                chip_ = idx - 1;
                chip_anim_.set(static_cast<float>(idx), visible() && !sh_.test_mode);
                if (visible() && !sh_.test_mode) start_anim();
                if (!trim(field_.text).empty()) run_search(0);
                break;
            case IdHelpBtn:
                help_pinned_ = !help_pinned_;
                drop_open_ = false;
                drop_from_button_ = false;
                apply_height();
                break;
            case IdHelpItem:
                insert_help_syntax(idx);
                return;
            case IdTagBtn:
                help_pinned_ = false;
                drop_from_button_ = !(drop_open_ && drop_from_button_);
                if (drop_from_button_) {
                    sh_.refresh_tags();
                    update_dropdown();
                } else {
                    drop_open_ = false;
                    apply_height();
                }
                break;
            case IdNotifyBtn:
            case IdStatusNotify:
                if (sh_.open_notifications) {
                    sh_.open_notifications();
                } else if (sh_.open_settings) {
                    sh_.open_settings();
                }
                return;
            case IdZoomReset: set_grid_zoom(GRID_ZOOM_MIN); return;
            case IdSettings:
                if (sh_.open_settings) {
                    sh_.open_settings();
                } else {
                    hide();
                }
                return;
            case IdFullscreen: toggle_fullscreen(); return;
            case IdClose: hide(); return;
            case IdSimilar: find_similar(items_[idx]); return;
            case IdCardPreview:
                if (parse_thumb_dblclick(sh_.settings.thumb_dblclick) == ThumbDblClick::Preview) {
                    open_item_file(item_path(items_[idx]));
                } else {
                    open_preview(idx);
                }
                return;
            case IdCard:
            case IdCardName:
            case IdCardDir:
            case IdCardMoreTags:
            case IdTagChip: {
                const uint64_t cid = items_[idx].pt.id;
                if (dbl) {
                    if (parse_thumb_dblclick(sh_.settings.thumb_dblclick) == ThumbDblClick::Preview) {
                        open_preview(idx);
                    } else {
                        open_item_file(item_path(items_[idx]));
                    }
                    return;
                }
                if (ctrl) {
                    if (!selected_.erase(cid)) selected_.insert(cid);
                } else if (shift && focus_ >= 0 && std::cmp_less(focus_, items_.size())) {
                    selected_.clear();
                    for (int k = std::min(focus_, idx); k <= std::max(focus_, idx); ++k) selected_.insert(items_[k].pt.id);
                } else if (selected_.contains(cid) && selected_.size() > 1) {
                    deferred_select_ = cid; // keep the selection in case this press starts a drag
                } else {
                    selected_ = {cid};
                }
                if (!shift) focus_ = idx;
                field_.focused = false; // keys now act on the cards (Space, Ctrl+C, Ctrl+A)
                begin_drag_detect(idx);
                break;
            }
            case IdScrollbar:
                if (contains(scroll_thumb_, p)) {
                    dragging_scroll_ = true;
                    drag_y_ = p.y;
                    drag_scroll_start_ = scroll_;
                } else {
                    const Grid g = grid();
                    target_scroll_ = std::clamp(((p.y - g.view.top) / height(g.view) * grid().content_h) - (height(g.view) / 2), 0.0f, max_scroll());
                    start_anim();
                }
                SetCapture(hwnd_);
                break;
            case IdMenuItem: menu_action(idx); return;
            case IdMenuTag: {
                const std::string t = menu_tags_[idx];
                set_tag(t, tag_state(t) != 2);
                return;
            }
            case IdMenuField: {
                menu_field_.focused = true;
                Canvas cv{target_, sh_.fonts, hits_, sh_.theme};
                menu_field_.click(cv, p.x, shift);
                break;
            }
            case IdDropItem: accept_dropdown(idx); return;
            case IdGrid:
                selected_.clear();
                focus_ = -1;
                break;
            default: break;
        }
        invalidate();
    }

    void on_rbutton_down(D2D1_POINT_2F p) {
        flush();
        clear_tooltip_now();
        const Hit* h = hit_test(hits_, p);
        if (preview_valid()) {
            if (h && (h->id == IdPvImage || (h->id == IdPvPlay && items_[preview_].pt.p.k == 1 && video_.failed()))) {
                selected_ = {items_[preview_].pt.id};
                open_menu(preview_, p);
            } else if (menu_open_) {
                close_popups();
                invalidate();
            }
            return;
        }
        if (!h || h->id < IdCard || h->id > IdTagChip) {
            if (menu_open_) {
                close_popups();
                invalidate();
            }
            return;
        }
        if (!selected_.contains(items_[h->index].pt.id)) selected_ = {items_[h->index].pt.id};
        focus_ = h->index;
        open_menu(h->index, p);
    }

    void on_wheel(int delta) {
        clear_tooltip_now();
        if (menu_open_) return;
        if (preview_valid()) {
            flush();
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            const D2D1_POINT_2F cur_p{.x = static_cast<float>(pt.x) / target_.scale(), .y = static_cast<float>(pt.y) / target_.scale()};
            const Hit* h = hit_test(hits_, cur_p);
            const int id = h ? h->id : IdNone;
            if ((id == IdPvMute || id == IdPvVolume) && items_[preview_].pt.p.k == 1 && !video_.failed()) {
                nudge_video_volume((static_cast<double>(delta) / static_cast<double>(WHEEL_DELTA)) * 0.05);
                return;
            }
            // One notch per image, whatever the device's resolution
            preview_wheel_ += delta;
            while (preview_wheel_ >= WHEEL_DELTA) {
                preview_wheel_ -= WHEEL_DELTA;
                preview_step(-1);
            }
            while (preview_wheel_ <= -WHEEL_DELTA) {
                preview_wheel_ += WHEEL_DELTA;
                preview_step(1);
            }
            return;
        }
        if (!expanded()) return;
        if (GetKeyState(VK_CONTROL) < 0 && !drop_open_) {
            grid_wheel_ += delta;
            while (grid_wheel_ >= WHEEL_DELTA) {
                grid_wheel_ -= WHEEL_DELTA;
                step_grid_zoom(1);
            }
            while (grid_wheel_ <= -WHEEL_DELTA) {
                grid_wheel_ += WHEEL_DELTA;
                step_grid_zoom(-1);
            }
            return;
        }
        target_scroll_ = std::clamp(target_scroll_ - (static_cast<float>(delta) * 0.9f), 0.0f, max_scroll());
        start_anim();
    }

    void move_focus(int dx, int dy) {
        if (items_.empty()) return;
        const Grid g = grid();
        int f = focus_ < 0 ? 0 : focus_ + dx + (dy * g.cols);
        f = std::clamp(f, 0, static_cast<int>(items_.size()) - 1);
        focus_ = f;
        if (GetKeyState(VK_SHIFT) < 0) {
            selected_.insert(items_[f].pt.id);
        } else {
            selected_ = {items_[f].pt.id};
        }
        field_.focused = false;
        scroll_to(f);
        invalidate();
    }

    void on_key(UINT vk) {
        clear_tooltip_now();
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        // A bare modifier press must not pull focus back to the search field before the chord's key arrives
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL) return;
        if (vk == VK_ESCAPE) {
            if (menu_open_ || drop_open_ || help_pinned_) {
                close_popups();
            } else if (preview_ >= 0) {
                close_preview();
            } else if (fullscreen_) {
                toggle_fullscreen();
            } else if (!selected_.empty() && !field_.focused) {
                selected_.clear();
                focus_ = -1;
                field_.focused = true;
            } else {
                hide();
            }
            invalidate();
            return;
        }
        if (vk == VK_F11) {
            toggle_fullscreen();
            return;
        }
        if (menu_open_) {
            bool handled = false;
            if (vk == VK_RETURN) {
                const std::string t = tag_key(menu_field_.text);
                if (!t.empty()) {
                    set_tag(t, true);
                    menu_field_.set(L"");
                } else {
                    menu_action(ActOpen);
                }
            } else {
                menu_field_.on_key(hwnd_, vk, handled);
            }
            caret_reset();
            invalidate();
            return;
        }
        if (preview_valid()) {
            preview_key(vk);
            invalidate();
            return;
        }
        if (drop_open_ && !drop_items_.empty()) {
            if (vk == VK_DOWN || vk == VK_UP) {
                drop_sel_ = (drop_sel_ + (vk == VK_DOWN ? 1 : -1) + static_cast<int>(drop_items_.size())) % static_cast<int>(drop_items_.size());
                invalidate();
                return;
            }
            if (vk == VK_RETURN || vk == VK_TAB) {
                accept_dropdown(drop_sel_);
                return;
            }
        }
        if (ctrl && !selected_.empty()) {
            if (vk == 'E') { menu_action_selected(ActReveal); return; }
            if (vk == 'S') { menu_action_selected(ActSimilar); return; }
            if (vk == 'C' && !field_.focused) { menu_action_selected(shift ? ActCopyPath : ActCopy); return; }
            if (vk == 'A' && !field_.focused) {
                for (const auto& it : items_) selected_.insert(it.pt.id);
                invalidate();
                return;
            }
        }
        if (ctrl && vk == 'F') {
            field_.focused = true;
            field_.select_all();
            invalidate();
            return;
        }
        if (ctrl && expanded()) {
            if (vk == VK_OEM_PLUS || vk == VK_ADD) { step_grid_zoom(1); return; }
            if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { step_grid_zoom(-1); return; }
            if (vk == '0' || vk == VK_NUMPAD0) { set_grid_zoom(GRID_ZOOM_MIN); return; }
        }
        if (vk == VK_SPACE && !field_.focused && focus_ >= 0 && std::cmp_less(focus_, items_.size())) {
            swallow_space_ = true;
            open_preview(focus_);
            return;
        }
        if (vk == VK_RETURN) {
            if (!field_.focused && focus_ >= 0) {
                menu_action_selected(ActOpen);
            } else {
                run_search(0);
            }
            return;
        }
        if (vk == VK_DOWN || (vk == VK_UP && !field_.focused) || (!field_.focused && (vk == VK_LEFT || vk == VK_RIGHT))) {
            if (vk == VK_UP && focus_ >= 0 && focus_ < grid().cols) {
                field_.focused = true;
                focus_ = -1;
                selected_.clear();
                invalidate();
                return;
            }
            move_focus(vk == VK_LEFT ? -1 : vk == VK_RIGHT ? 1 : 0, vk == VK_DOWN ? 1 : vk == VK_UP ? -1 : 0);
            return;
        }
        if (!field_.focused) {
            field_.focused = true;
            focus_ = -1;
        }
        bool handled = false;
        if (field_.on_key(hwnd_, vk, handled)) {
            schedule_search();
        } else {
            update_dropdown();
        }
        caret_reset();
        invalidate();
    }

    void menu_action_selected(int a) {
        menu_index_ = focus_;
        menu_action(a);
    }

    void on_char(wchar_t c) {
        if (swallow_space_) {
            swallow_space_ = false;
            if (c == L' ') return;
        }
        if (c == L'\r' || c == 27 || c == L'\t') return;
        if (menu_open_) {
            menu_field_.on_char(c);
            caret_reset();
            invalidate();
            return;
        }
        if (preview_valid()) return;
        if (!field_.focused) {
            field_.focused = true;
            focus_ = -1;
        }
        if (field_.on_char(c)) schedule_search();
        caret_reset();
        invalidate();
    }

    LRESULT hit_test_nc(LPARAM lp) {
        flush();
        POINT pt{.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (fullscreen_) return HTCLIENT;
        const float s = target_.scale();
        const D2D1_POINT_2F p{.x = static_cast<float>(pt.x) / s, .y = static_cast<float>(pt.y) / s};
        const D2D1_RECT_F pr = pill_bounds(target_.size());
        if (custom_pill_frame()) {
            constexpr float margin = 2.0f;
            if (p.x < pr.left - margin || p.x >= pr.right + margin || p.y < pr.top - margin || p.y >= pr.bottom + margin) {
                return HTNOWHERE;
            }
        }
        constexpr float b = 6.0f;
        const bool l = p.x < pr.left + b;
        const bool r = p.x >= pr.right - b;
        const bool t = p.y < pr.top + b;
        const bool btm = p.y >= pr.bottom - b;
        const bool vert = expanded();
        if (t && l && vert) return HTTOPLEFT;
        if (t && r && vert) return HTTOPRIGHT;
        if (btm && l && vert) return HTBOTTOMLEFT;
        if (btm && r && vert) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t && vert) return HTTOP;
        if (btm && vert) return HTBOTTOM;
        if (p.y < pr.top + HEADER_H + STATUS_H && !menu_open_ && !drop_open_ && !show_help()) {
            const Hit* h = hit_test(hits_, p);
            if (!h) return HTCAPTION;
        }
        return HTCLIENT;
    }

    // ------------------------------------------------------------------------------------------
    // Window procedure
    // ------------------------------------------------------------------------------------------

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<Pill*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (auto* self = reinterpret_cast<Pill*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) return self->handle(msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

public:
    std::function<LRESULT(UINT, WPARAM, LPARAM, bool&)> extra_handler;

private:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (extra_handler) {
            bool done = false;
            const LRESULT r = extra_handler(msg, wp, lp, done);
            if (done) return r;
        }
        switch (msg) {
            case WM_NCCALCSIZE:
                if (wp) return 0;
                break;
            case WM_NCHITTEST: return hit_test_nc(lp);
            case WM_NCLBUTTONDBLCLK:
                if (wp == HTCAPTION) {
                    toggle_fullscreen();
                    return 0;
                }
                break;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd_, &ps);
                if (visible() || sh_.test_mode) render();
                EndPaint(hwnd_, &ps);
                if (visible() && !sh_.test_mode && (fading_out_ || std::abs(target_fade_ - fade_) >= 0.005f)) {
                    on_fade_timer();
                }
                if (visible() && anim_active_) {
                    // Drain any due timers while QS_PAINT is clear so VSync frames don't starve fade/tooltip timers.
                    MSG m;
                    while (PeekMessageW(&m, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                        DispatchMessageW(&m);
                    }
                    if (visible() && anim_active_) InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            case WM_WINDOWPOSCHANGING: {
                if (glass_active() && visible() && in_sizemove_) {
                    const auto* wpos = reinterpret_cast<const WINDOWPOS*>(lp);
                    if (wpos) {
                        RECT win_rc{};
                        GetWindowRect(hwnd_, &win_rc);
                        const bool size_same = (wpos->flags & SWP_NOSIZE) ||
                            (wpos->cx == (win_rc.right - win_rc.left) && wpos->cy == (win_rc.bottom - win_rc.top));
                        resize_pending_ = !size_same;
                        if (size_same && !(wpos->flags & SWP_NOMOVE)) {
                            POINT cur_origin{.x = 0, .y = 0};
                            ClientToScreen(hwnd_, &cur_origin);
                            const POINT next_origin{
                                .x = wpos->x + (cur_origin.x - win_rc.left),
                                .y = wpos->y + (cur_origin.y - win_rc.top),
                            };
                            if (next_origin.x != glass_.last_rendered_origin().x ||
                                next_origin.y != glass_.last_rendered_origin().y) {
                                glass_.set_origin_override(next_origin);
                                full_dirty_ = true;
                                render();
                                glass_.set_origin_override(std::nullopt);
                                if (visible() && !anim_active_) ValidateRect(hwnd_, nullptr);
                            }
                        }
                    }
                }
                break;
            }
            case WM_SIZE:
                resize_pending_ = false;
                if (wp != SIZE_MINIMIZED) {
                    target_.resize(LOWORD(lp), HIWORD(lp));
                    scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
                    full_dirty_ = true;
                    render();
                    if (glass_active() && visible() && !anim_active_) ValidateRect(hwnd_, nullptr);
                }
                return 0;
            case WM_MOVE:
                if (glass_active() && visible()) {
                    if (in_sizemove_ && resize_pending_) return 0;
                    POINT cur_origin{.x = 0, .y = 0};
                    ClientToScreen(hwnd_, &cur_origin);
                    if (cur_origin.x != glass_.last_rendered_origin().x ||
                        cur_origin.y != glass_.last_rendered_origin().y) {
                        full_dirty_ = true;
                        if (in_sizemove_) {
                            render();
                        } else {
                            invalidate();
                        }
                    }
                }
                return 0;
            case WM_ENTERSIZEMOVE:
                in_sizemove_ = true;
                resize_pending_ = false;
                if (glass_active()) glass_.set_sizemove(true, sh_.desktop_dupl, sh_.gfx);
                return 0;
            case WM_EXITSIZEMOVE:
                in_sizemove_ = false;
                resize_pending_ = false;
                glass_.set_sizemove(false, sh_.desktop_dupl, sh_.gfx); // unconditional: always balance the freeze
                save_rect();
                if (sh_.gfx.adapter_stale(hwnd_)) recover();
                if (glass_active()) (void)sh_.poll_indexer();
                full_dirty_ = true;
                render();
                if (glass_active() && visible() && !anim_active_) ValidateRect(hwnd_, nullptr);
                return 0;
            case WM_DISPLAYCHANGE:
                if (sh_.gfx.adapter_stale(hwnd_)) recover();
                invalidate();
                return 0;
            case WM_SETTINGCHANGE:
                if (!lp || _wcsicmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                    sh_.on_windows_theme_changed();
                }
                return 0;
            case WM_DWMCOLORIZATIONCOLORCHANGED:
                sh_.on_windows_theme_changed();
                return 0;
            case WM_DPICHANGED: {
                full_dirty_ = true;
                if (placing_) {
                    target_.sync_dpi();
                    return 0; // our own SetWindowPos already sized the window for the new DPI
                }
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                if (fullscreen_) {
                    // Keep covering the monitor rather than taking the scaled-down suggestion.
                    MONITORINFO mi{.cbSize = sizeof(mi)};
                    GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi);
                    const RECT& m = mi.rcMonitor;
                    SetWindowPos(hwnd_, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOZORDER | SWP_NOACTIVATE);
                    target_.sync_dpi();
                    invalidate();
                    return 0;
                }
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                target_.sync_dpi();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                if (fullscreen_) return 0;
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                const UINT dpi = placing_dpi_ ? placing_dpi_ : GetDpiForWindow(hwnd_);
                const float s = static_cast<float>(dpi > 0 ? dpi : 96u) / 96.0f;
                const ShadowPad pad = active_pad();
                mmi->ptMinTrackSize = {
                    .x = static_cast<LONG>((520.0f + pad.left + pad.right) * s),
                    .y = static_cast<LONG>((HEADER_H + STATUS_H + pad.top + pad.bottom) * s),
                };
                return 0;
            }
            case WM_ACTIVATE: {
                const bool active = LOWORD(wp) != WA_INACTIVE;
                if (window_active_ != active) {
                    window_active_ = active;
                    if (custom_pill_frame() && visible()) {
                        full_dirty_ = true;
                        invalidate();
                    }
                }
                if (!active && sh_.settings.hide_on_focus_loss && !fullscreen_ && !sh_.test_mode && !dragging_out_ &&
                    GetTickCount64() - shown_tick_ > 250) {
                    if (visible() && !fading_out_) {
                        GetCursorPos(&blur_hide_cursor_);
                        blur_hide_tick_ = GetTickCount64();
                    }
                    hide();
                }
                return 0;
            }
            case WM_CAPTURECHANGED:
                selecting_text_ = false;
                dragging_scroll_ = false;
                end_seek_drag();
                end_volume_drag();
                end_drag_detect();
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    if (copy_decoding_) {
                        SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
                        return TRUE;
                    }
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd_, &pt);
                    const Hit* h = hit_test(hits_, {.x = static_cast<float>(pt.x) / target_.scale(), .y = static_cast<float>(pt.y) / target_.scale()});
                    const int id = h ? h->id : 0;
                    SetCursor(id == IdField || id == IdMenuField ? cursor_ibeam_ : (id == IdNone || id == IdGrid || id == IdMenu || id == IdDrop || id == IdHelp || id == IdCard || id == IdCardName || id == IdCardDir || id == IdPreview || id == IdPvImage || id == IdPvPath || id == IdPvBar) ? cursor_arrow_ : cursor_hand_);
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: on_mouse_move(to_dip(lp)); return 0;
            case WM_MOUSELEAVE: {
                tracking_ = false;
                const bool animate = visible() && !sh_.test_mode;
                tooltip_.clear(hwnd_, TIMER_TOOLTIP, animate);
                if (raw_hover_id_ != IdNone || hover_.id != IdNone || card_hover_.id != IdNone) {
                    const bool was_help = show_help();
                    clear_hover(animate);
                    if (show_help() != was_help) {
                        full_dirty_ = true;
                        apply_height();
                    }
                    if (animate) {
                        start_anim();
                    } else {
                        invalidate();
                    }
                } else if (animate && tooltip_.alpha.active()) {
                    start_anim();
                }
                return 0;
            }
            case WM_LBUTTONDOWN: on_lbutton_down(to_dip(lp), false); return 0;
            case WM_LBUTTONDBLCLK: on_lbutton_down(to_dip(lp), true); return 0;
            case WM_LBUTTONUP:
                selecting_text_ = false;
                if (dragging_scroll_) dragging_scroll_ = false;
                if (dragging_seek_) end_seek_drag();
                if (dragging_volume_) end_volume_drag();
                end_drag_detect();
                ReleaseCapture();
                return 0;
            case WM_RBUTTONDOWN: on_rbutton_down(to_dip(lp)); return 0;
            case WM_MOUSEWHEEL: on_wheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
                on_key(static_cast<UINT>(wp));
                return 0;
            case WM_CHAR: on_char(static_cast<wchar_t>(wp)); return 0;
            case WM_SYSCHAR: return 0;
            case WM_IME_STARTCOMPOSITION:
                (menu_open_ ? menu_field_ : field_).place_ime(hwnd_, target_.scale());
                break;
            case WM_TIMER:
                switch (wp) {
                    case TIMER_CARET:
                        if (glass_active() && in_sizemove_) return 0;
                        field_.caret_on = !field_.caret_on;
                        menu_field_.caret_on = field_.caret_on;
                        if (field_.focused || menu_open_) invalidate();
                        return 0;
                    case TIMER_DEBOUNCE: run_search(0); return 0;
                    case TIMER_STATUS:
                        if (!(glass_active() && in_sizemove_) && sh_.poll_indexer()) invalidate();
                        return 0;
                    case TIMER_DEVICE:
                        KillTimer(hwnd_, TIMER_DEVICE);
                        full_dirty_ = true;
                        render();
                        return 0;
                    case TIMER_FADE:
                        on_fade_timer();
                        return 0;
                    case TIMER_DROP: {
                        const size_t before = async_drops_.size();
                        std::erase_if(async_drops_, [](const auto& drop) {
                            BOOL in_operation = FALSE;
                            return FAILED(drop->InOperation(&in_operation)) || !in_operation;
                        });
                        sh_.background_jobs -= static_cast<int>(before - async_drops_.size());
                        if (async_drops_.empty()) {
                            KillTimer(hwnd_, TIMER_DROP);
                            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                            if (sh_.visibility_changed) sh_.visibility_changed();
                        }
                        return 0;
                    }
                    case TIMER_COPY: set_clipboard(); return 0;
                    case TIMER_TOOLTIP: {
                        std::wstring tt;
                        std::wstring ts;
                        const bool allow = raw_hover_id_ != IdNone && !menu_open_ && !drop_open_ && !drag_pending_ && !dragging_out_ && tooltip_for(raw_hover_id_, raw_hover_index_, tt, ts);
                        const bool animate = visible() && !sh_.test_mode;
                        if (tooltip_.on_timer(hwnd_, TIMER_TOOLTIP, allow, std::move(tt), std::move(ts), animate)) {
                            if (animate) {
                                start_anim();
                            } else {
                                invalidate();
                            }
                        }
                        return 0;
                    }
                    case TIMER_GIF: on_gif_timer(); return 0;
                    case TIMER_VIDEO: on_video_timer(); return 0;
                    case TIMER_GLASS:
                        if (glass_active() && visible() && glass_.poll_desktop(sh_.desktop_dupl, sh_.gfx, hwnd_)) {
                            full_dirty_ = true;
                            invalidate();
                        }
                        return 0;
                    default:
                        break;
                }
                return 0;
            case WM_APP_THUMBS: on_thumbs(); return 0;
            case WM_APP_GIF: on_gif_worker(); return 0;
            case WM_APP_VIDEO: on_video_event(); return 0;
            case WM_APP_CALL:
                if (wp == 1) {
                    run_search(pagination_.next_offset());
                } else if (wp == 2) {
                    invalidate();
                } else if (lp) {
                    const std::unique_ptr<std::function<void()>> fn{reinterpret_cast<std::function<void()>*>(lp)};
                    (*fn)();
                }
                return 0;
            case WM_CLOSE: hide(); return 0;
            default: break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    Shared& sh_;
    HWND hwnd_ = nullptr;
    WindowTarget target_;
    LiquidGlassWindow glass_;
    std::vector<Hit> hits_;
    std::vector<Hit> partial_hits_;
    std::vector<int> touched_cards_scratch_;
    std::vector<Hit> overlays_scratch_;
    std::string cached_model_utf8_;
    std::wstring cached_model_w_;
    std::wstring pv_dir_disp_;
    float pv_dir_disp_w_ = -1.0f;
    uint64_t pv_dir_disp_id_ = 0;
    HCURSOR cursor_arrow_ = nullptr, cursor_hand_ = nullptr, cursor_ibeam_ = nullptr;
    bool rendering_ = false;
    bool dirty_ = false; // state changed since the last frame
    bool full_dirty_ = true;
    std::vector<D2D1_RECT_F> pending_dirty_;
    std::vector<D2D1_RECT_F> dirty_dips_scratch_;
    std::optional<D2D1_RECT_F> last_tooltip_dirty_;
    ULONGLONG shown_tick_ = 0;
    ULONGLONG blur_hide_tick_ = 0;
    POINT blur_hide_cursor_{};

    TextField field_;
    int chip_ = -1;
    AnimFloat chip_anim_;
    std::wstring last_query_;
    uint64_t seq_ = 0;
    bool debounce_pending_ = false;
    bool searching_ = false;
    bool loading_more_ = false;
    bool has_more_ = false;
    bool semantic_ = false;
    double last_ms_ = 0;
    std::wstring error_;
    std::vector<Item> items_;
    PaginationTracker pagination_;

    std::unordered_set<uint64_t> selected_;
    int focus_ = -1;
    int raw_hover_id_ = IdNone;
    int raw_hover_index_ = 0;
    HoverTracker hover_;
    HoverTracker card_hover_;
    HoverTooltip tooltip_;
    bool tracking_ = false;
    bool anim_active_ = false;
    bool selecting_text_ = false;

    float scroll_ = 0, target_scroll_ = 0;
    float fade_ = 1.0f, target_fade_ = 1.0f;
    BYTE last_alpha_ = 255;
    bool fading_out_ = false;
    std::chrono::steady_clock::time_point last_anim_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_fade_ = std::chrono::steady_clock::now();
    bool dragging_scroll_ = false;
    float drag_y_ = 0, drag_scroll_start_ = 0;
    D2D1_RECT_F scroll_thumb_{};

    bool fullscreen_ = false;
    bool window_active_ = false;
    ShadowPad applied_pad_{};
    bool placing_ = false;
    UINT placing_dpi_ = 0;
    RECT restore_rect_{};
    UINT restore_dpi_ = 96;
    bool in_sizemove_ = false;
    bool resize_pending_ = false;

    bool menu_open_ = false;
    int menu_index_ = -1;
    D2D1_POINT_2F menu_at_{};
    TextField menu_field_;
    std::vector<std::string> menu_tags_;

    bool drop_open_ = false;
    bool drop_from_button_ = false;
    std::vector<TagCount> drop_items_;
    int drop_sel_ = 0;

    bool help_pinned_ = false;
    float help_btn_x_ = 0.0f;

    int preview_ = -1;       // item shown full size, -1 when the viewer is closed
    int preview_initial_index_ = -1;
    float preview_initial_scroll_ = 0.0f;
    int preview_wheel_ = 0;  // wheel delta not yet turned into a step
    int grid_wheel_ = 0;     // ctrl+wheel delta for grid zoom
    UINT thumb_px_ = 0;      // card thumbnail size last requested by the grid
    bool swallow_space_ = false;
    bool swallow_dbl_ = false;

    uint64_t gfx_epoch_ = 0;
    bool deferred_thumbs_ = false;
    bool preview_gpu_reset_ = false;
    bool video_timer_active_ = false;
    bool dragging_seek_ = false;
    bool dragging_volume_ = false;
    bool gif_scrub_resume_ = false;
    float seek_drag_ratio_ = 0.0f;
    D2D1_RECT_F pv_media_rect_{};
    D2D1_RECT_F pv_ctrl_rect_{};
    D2D1_RECT_F pv_seek_rect_{};
    D2D1_RECT_F pv_vol_rect_{};
    std::chrono::steady_clock::time_point last_scrubber_redraw_{};
    GifPlayer gif_;
    VideoPlayer video_;

    bool drag_pending_ = false; // left button down on media, not yet past the drag threshold
    bool dragging_out_ = false;
    int drag_index_ = -1;
    POINT drag_origin_{};
    std::optional<uint64_t> deferred_select_;
    std::vector<wil::com_ptr<IDataObjectAsyncCapability>> async_drops_; // drops the target is still completing

    uint64_t copy_generation_ = 0;
    bool copy_decoding_ = false;
    wil::com_ptr<IDataObject> pending_clipboard_;
    int clipboard_attempts_ = 0;
    std::map<uint64_t, std::jthread> copy_workers_; // last member: joined before the state they report to goes away
};

} // namespace usyseek::ui
