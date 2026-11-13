#pragma once

#include "gif_player.hpp"
#include "pagination.hpp"
#include "shared.hpp"
#include "textfield.hpp"
#include "video_player.hpp"

#include <shellscalingapi.h>

#include <algorithm>
#include <cstdlib>
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
        WNDCLASSEXW wc{sizeof(wc)};
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
        style_window(hwnd_, DWMSBT_TRANSIENTWINDOW);
        const COLORREF border = RGB(0x44, 0x41, 0x48);
        DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
        if (!target_.attach(sh_.gfx, hwnd_)) return false;
        gfx_epoch_ = sh_.gfx.epoch();
        gif_.set_notify(hwnd_, WM_APP_GIF);
        video_.set_notify(hwnd_, WM_APP_VIDEO);
        video_.set_muted(sh_.test_mode || video_muted_);

        field_.placeholder = L"Search images & videos";
        cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
        cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
        return true;
    }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool visible() const noexcept { return IsWindowVisible(hwnd_) != FALSE; }
    WindowTarget& target() noexcept { return target_; }

    void show() {
        blur_hide_tick_ = 0;
        if (!visible()) {
            place();
            fade_ = sh_.test_mode ? 1.0f : 0.0f;
        }
        fading_out_ = false;
        target_fade_ = 1.0f;
        apply_fade();
        shown_tick_ = GetTickCount64();
        selecting_text_ = false;
        dragging_scroll_ = false;
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        field_.focused = !preview_valid();
        if (field_.focused) field_.select_all();
        caret_reset();
        sh_.refresh_tags();
        sh_.poll_indexer(true);
        sh_.refresh_hotkey_state();
        if (preview_valid()) request_preview();
        SetTimer(hwnd_, TIMER_STATUS, 1000, nullptr);
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
        KillTimer(hwnd_, TIMER_VIDEO);
        dragging_seek_ = false;
        gif_.close();
        video_.close();
        sh_.previews.clear();
        anim_active_ = false;
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        hover_.set(IdNone, 0, false);
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
        ReleaseCapture();
        close_popups();
        KillTimer(hwnd_, TIMER_CARET);
        KillTimer(hwnd_, TIMER_STATUS);
        KillTimer(hwnd_, TIMER_GIF);
        KillTimer(hwnd_, TIMER_VIDEO);
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
    void reset_preview_gpu() noexcept {
        gif_.reset_gpu();
        video_.reset_gpu();
    }

    /// COM objects this window still holds; released before OLE is uninitialised.
    void release_com_objects() {
        async_drops_.clear();
        pending_clipboard_.reset();
        gif_.shutdown();
        video_.shutdown();
    }

    void toggle() {
        if (visible() && !fading_out_ && GetForegroundWindow() == hwnd_) hide();
        else show();
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
    void invalidate() {
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
            chip_anim_.active() || searching_ || loading_more_ || preview_loading()) {
            full_dirty_ = true;
        }

        std::vector<D2D1_RECT_F> dirty_dips;
        if (!full_dirty_) {
            dirty_dips = std::exchange(pending_dirty_, {});
            if (hover_.in_anim.value != hover_.in_anim.target) queue_hover_dirty(dirty_dips, hover_.id, hover_.index, full_dirty_);
            if (hover_.prev_id != IdNone && hover_.out_anim.value != hover_.out_anim.target) {
                queue_hover_dirty(dirty_dips, hover_.prev_id, hover_.prev_index, full_dirty_);
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
            if (curr_tip_dirty) dirty_dips.push_back(*curr_tip_dirty);
            if (last_tooltip_dirty_ && (!curr_tip_dirty || !rect_equal(*last_tooltip_dirty_, *curr_tip_dirty))) {
                dirty_dips.push_back(*last_tooltip_dirty_);
            }
        }

        const bool request_full = std::exchange(full_dirty_, false);
        std::vector<Hit> partial_hits;
        bool drew_full = false;
        const bool ok = target_.render_frame(request_full, dirty_dips, !in_sizemove_, [&](std::optional<D2D1_RECT_F> clip, bool full_frame) {
            if (full_frame) {
                drew_full = true;
                hits_.clear();
                Canvas cv{target_, sh_.fonts, hits_, clip};
                draw(cv);
            } else {
                Canvas cv{target_, sh_.fonts, partial_hits, clip};
                draw(cv);
            }
        });
        if (!ok) recover();
        if (!drew_full && !partial_hits.empty()) sync_card_overlay_hits(hits_, partial_hits);
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
        if (index >= static_cast<int>(items_.size())) return;
        selected_ = {items_[index].pt.id};
        focus_ = index;
        open_menu(index, {420.0f, 260.0f});
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
        if (index < 0) close_preview();
        else open_preview(index);
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
    static constexpr float DEFAULT_W = 880.0f;
    static constexpr float DEFAULT_H = 640.0f;
    static constexpr float MENU_W = 256.0f;
    static constexpr UINT_PTR TIMER_CARET = 1, TIMER_DEBOUNCE = 2, TIMER_STATUS = 4, TIMER_DEVICE = 5, TIMER_FADE = 6, TIMER_DROP = 7, TIMER_COPY = 8, TIMER_TOOLTIP = 9, TIMER_GIF = 10, TIMER_VIDEO = 11;
    static constexpr uint64_t COPY_MAX_PIXELS = 40'000'000;
    static constexpr int CLIPBOARD_ATTEMPTS = 10;

    enum Id : uint8_t {
        IdNone = 0,
        IdField,
        IdChip,
        IdTagBtn,
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
        std::vector<std::wstring> tags_w;
    };

    static Item make_item(DbPoint p) {
        Item it;
        const std::wstring path = to_wide(p.p.p);
        const auto slash = path.find_last_of(L'\\');
        it.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        it.dir = slash == std::wstring::npos ? L"" : path.substr(0, slash);
        std::wstring meta;
        if (p.p.w && p.p.h) meta = std::to_wstring(p.p.w) + L"×" + std::to_wstring(p.p.h) + L"  •  ";
        meta += format_size(p.p.s);
        it.meta = meta;
        it.date = format_date(p.p.m);
        for (const auto& t : p.p.t) it.tags_w.push_back(to_wide(t));
        it.pt = std::move(p);
        return it;
    }

    [[nodiscard]] std::wstring item_path(const Item& it) const { return to_wide(it.pt.p.p); }

    // ------------------------------------------------------------------------------------------
    // Window placement
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] bool expanded() const noexcept { return !field_.text.empty() || !items_.empty() || fullscreen_; }

    void place() {
        if (fullscreen_) return;
        POINT cur;
        GetCursorPos(&cur);
        HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
        if (sh_.settings.pill_x != INT32_MIN) {
            const POINT saved{sh_.settings.pill_x, sh_.settings.pill_y};
            if (HMONITOR m = MonitorFromPoint(saved, MONITOR_DEFAULTTONULL)) mon = m;
        }
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        UINT dpi_x = 96, dpi_y = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
        const float s = static_cast<float>(dpi_x) / 96.0f;
        const int w = static_cast<int>((sh_.settings.pill_w > 0 ? sh_.settings.pill_w : DEFAULT_W) * s);
        const int h = static_cast<int>(current_height_dip() * s);
        int x, y;
        if (sh_.settings.pill_x != INT32_MIN) {
            x = sh_.settings.pill_x;
            y = sh_.settings.pill_y;
        } else {
            const RECT& wa = mi.rcWork;
            x = wa.left + (wa.right - wa.left - w) / 2;
            y = wa.top + (wa.bottom - wa.top) / 5;
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    }

    static constexpr float HELP_W = 420.0f;
    static constexpr float HELP_H = 240.0f;

    [[nodiscard]] bool show_help() const noexcept {
        return help_pinned_ || raw_hover_id_ == IdHelpBtn || raw_hover_id_ == IdHelp || raw_hover_id_ == IdHelpItem;
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

    static void sync_card_overlay_hits(std::vector<Hit>& hits, const std::vector<Hit>& partial_hits) {
        std::vector<int> touched_cards;
        for (const Hit& h : partial_hits) {
            if (h.id == IdCard && std::ranges::find(touched_cards, h.index) == touched_cards.end()) {
                touched_cards.push_back(h.index);
            }
        }
        if (touched_cards.empty()) return;

        for (const int card_idx : touched_cards) {
            std::erase_if(hits, [card_idx](const Hit& h) {
                return h.index == card_idx && (h.id == IdSimilar || h.id == IdCardPreview);
            });

            std::vector<Hit> overlays;
            for (const Hit& ph : partial_hits) {
                if (ph.index == card_idx && (ph.id == IdSimilar || ph.id == IdCardPreview)) {
                    const bool dup = std::ranges::any_of(overlays, [&](const Hit& ex) {
                        return ex.id == ph.id && ex.index == ph.index;
                    });
                    if (!dup) overlays.push_back(ph);
                }
            }
            if (overlays.empty()) continue;

            const auto card_it = std::ranges::find_if(hits, [card_idx](const Hit& h) {
                return h.id == IdCard && h.index == card_idx;
            });
            if (card_it != hits.end()) {
                hits.insert(std::next(card_it), overlays.begin(), overlays.end());
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
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        hover_.set(IdNone, 0, animate);
    }

    [[nodiscard]] float help_height_dip() const noexcept {
        if (!show_help()) return 0.0f;
        return (HEADER_H - 4.0f) + HELP_H + 12.0f;
    }

    [[nodiscard]] float dropdown_height_dip() const noexcept {
        if (!drop_open_) return 0.0f;
        const float row = 32.0f;
        const float h = drop_items_.empty() ? 44.0f : 8.0f + row * static_cast<float>(drop_items_.size());
        return (HEADER_H - 4.0f) + h + 12.0f;
    }

    [[nodiscard]] float current_height_dip() const {
        if (!expanded()) return std::max({HEADER_H + STATUS_H, dropdown_height_dip(), help_height_dip()});
        return sh_.settings.pill_h > 0 ? static_cast<float>(sh_.settings.pill_h) : DEFAULT_H;
    }

    void apply_height() {
        if (fullscreen_ || in_sizemove_ || rendering_) return;
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        const int h = static_cast<int>(current_height_dip() * target_.scale());
        if (wr.bottom - wr.top != h) SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void save_rect() {
        if (fullscreen_) return;
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        sh_.settings.pill_x = wr.left;
        sh_.settings.pill_y = wr.top;
        sh_.settings.pill_w = static_cast<int32_t>((wr.right - wr.left) / target_.scale());
        if (expanded()) sh_.settings.pill_h = static_cast<int32_t>((wr.bottom - wr.top) / target_.scale());
        sh_.save();
    }

    void toggle_fullscreen() {
        if (!fullscreen_) {
            GetWindowRect(hwnd_, &restore_rect_);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            fullscreen_ = true;
            const DWM_WINDOW_CORNER_PREFERENCE c = DWMWCP_DONOTROUND;
            DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &c, sizeof(c));
            const RECT& r = mi.rcMonitor;
            SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
        } else {
            fullscreen_ = false;
            const DWM_WINDOW_CORNER_PREFERENCE c = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &c, sizeof(c));
            const RECT& r = restore_rect_;
            SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
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
        if (target_.ready() && gfx_epoch_ != sh_.gfx.epoch()) target_.release();
        if (!sh_.gfx.ready() || sh_.gfx.device_lost()) {
            reset_preview_gpu();
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
        return true;
    }

    void recover() {
        // Present failed: device removed/reset or adapter gone. Rebuild everything on the next frame.
        reset_preview_gpu();
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
        if (!target_.ready()) return;
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

    [[nodiscard]] Grid grid() const {
        Grid g;
        const D2D1_SIZE_F sz = target_.size();
        g.view = D2D1::RectF(0, HEADER_H + STATUS_H + 1.0f, sz.width, sz.height);
        const float avail = sz.width - PAD * 2;
        g.cols = std::max(1, static_cast<int>((avail + GAP) / (CARD_MIN_W + GAP)));
        g.card_w = std::floor((avail - GAP * (g.cols - 1)) / g.cols);
        g.thumb_h = std::round((g.card_w - 12.0f) * 10.0f / 16.0f);
        g.card_h = g.thumb_h + CARD_BODY_H;
        const int rows = (static_cast<int>(items_.size()) + g.cols - 1) / g.cols;
        g.content_h = PAD + rows * (g.card_h + GAP) - (rows ? GAP : 0) + PAD + (has_more_ ? 40.0f : 0.0f);
        return g;
    }

    [[nodiscard]] float max_scroll() const {
        const Grid g = grid();
        return std::max(0.0f, g.content_h - height(g.view));
    }

    [[nodiscard]] D2D1_RECT_F card_rect(const Grid& g, int i) const {
        const int row = i / g.cols, col = i % g.cols;
        const float x = PAD + col * (g.card_w + GAP);
        const float y = g.view.top + PAD + row * (g.card_h + GAP) - scroll_;
        return rect(x, y, g.card_w, g.card_h);
    }

    void scroll_to(int i, bool animate = true) {
        if (i < 0) return;
        const Grid g = grid();
        const D2D1_RECT_F r = card_rect(g, i);
        if (r.top < g.view.top + 4) target_scroll_ = scroll_ + (r.top - g.view.top - PAD);
        else if (r.bottom > g.view.bottom - 4) target_scroll_ = scroll_ + (r.bottom - g.view.bottom + PAD);
        target_scroll_ = std::clamp(target_scroll_, 0.0f, max_scroll());
        if (animate) start_anim();
        else scroll_ = target_scroll_;
    }

    void apply_fade() {
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
        if (target_fade_ > fade_) fade_ = std::min(target_fade_, fade_ + step);
        else fade_ = std::max(target_fade_, fade_ - step);
        apply_fade();
        if (std::abs(target_fade_ - fade_) < 0.005f) {
            fade_ = target_fade_;
            apply_fade();
            KillTimer(hwnd_, TIMER_FADE);
            if (fading_out_) finish_hide();
        }
    }

    void start_anim() {
        if (!visible()) return;
        if (!anim_active_) last_anim_ = std::chrono::steady_clock::now();
        anim_active_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool step_anim() {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_anim_).count(), 0.001f, 0.05f);
        last_anim_ = now;
        bool moving = false;
        const float d = target_scroll_ - scroll_;
        if (std::abs(d) < 0.5f) {
            scroll_ = target_scroll_;
        } else {
            scroll_ += d * (1.0f - std::exp(-dt * 22.0f));
            if (std::abs(target_scroll_ - scroll_) < 0.5f) scroll_ = target_scroll_;
            else moving = true;
        }
        if (chip_anim_.step(dt)) moving = true;
        if (hover_.step(dt)) moving = true;
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
            case IdHelpBtn: title = L"Search syntax & shortcuts"; return true;
            case IdSettings: title = L"Settings"; return true;
            case IdFullscreen: title = fullscreen_ ? L"Exit fullscreen (Esc)" : L"Fullscreen"; return true;
            case IdClose: title = L"Hide (Esc)"; return true;
            case IdCardPreview: title = L"Preview (Space)"; return true;
            case IdSimilar: title = L"Find similar (Ctrl+S)"; return true;
            case IdCardName:
            case IdCardDir:
                if (idx >= 0 && idx < static_cast<int>(items_.size())) {
                    title = items_[idx].name;
                    subtitle = items_[idx].dir;
                    return !title.empty() || !subtitle.empty();
                }
                return false;
            case IdCardMoreTags:
                if (idx >= 0 && idx < static_cast<int>(items_.size())) {
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
                        title = gif_.playing() ? L"Pause (K)" : L"Play (K)";
                        return true;
                    }
                }
                title = L"Open video";
                return true;
            case IdPvMute:
                title = video_.is_muted() ? L"Unmute (M)" : L"Mute (M)";
                return true;
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

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        cv.fill(D2D1::RectF(0, 0, sz.width, sz.height), with_alpha(color::bg, 0.80f));
        if (preview_ >= 0) {
            draw_preview(cv, sz);
            if (menu_open_) draw_menu(cv);
            draw_active_tooltip(cv, sz);
            return;
        }
        draw_header(cv, sz);
        draw_status(cv, sz);
        if (expanded()) draw_grid(cv);
        if (drop_open_) draw_dropdown(cv);
        if (show_help()) draw_help(cv, sz);
        if (menu_open_) draw_menu(cv);
        draw_active_tooltip(cv, sz);
    }

    void icon_button(Canvas& cv, D2D1_RECT_F r, wchar_t g, int id, bool active = false) {
        const float ha = hover_.alpha(id);
        if (active) cv.fill_round(r, height(r) / 2, with_alpha(color::primary, 0.16f));
        else if (ha > 0.002f) cv.fill_round(r, height(r) / 2, with_alpha(color::on_surface, 0.08f * ha));
        cv.icon(g, r, active ? color::primary : lerp_color(color::on_surface_variant, color::on_surface, ha));
        cv.hit(r, id);
    }

    void draw_header(Canvas& cv, D2D1_SIZE_F sz) {
        const float cy = HEADER_H / 2;
        // Search glyph with a status dot
        const D2D1_RECT_F ic = rect(16, cy - 12, 24, 24);
        cv.icon(glyph::search, ic, color::on_surface_variant, Font::IconLarge);
        const D2D1_COLOR_F dot = status_dot_color();
        cv.fill_circle({ic.left + 20, ic.top + 20}, 4.5f, color::bg);
        cv.fill_circle({ic.left + 20, ic.top + 20}, 3.0f, dot);

        // Right cluster: close, fullscreen, settings | tag, chips
        float x = sz.width - 12;
        auto btn = [&](wchar_t g, int id, bool active = false) {
            x -= 36;
            icon_button(cv, rect(x, cy - 18, 36, 36), g, id, active);
        };
        btn(glyph::close, IdClose);
        btn(fullscreen_ ? glyph::exit_fullscreen : glyph::fullscreen, IdFullscreen);
        btn(glyph::settings, IdSettings);
        x -= 8;
        cv.vline(x, cy - 12, cy + 12, color::outline_variant);
        x -= 4;
        btn(glyph::help, IdHelpBtn, show_help());
        help_btn_x_ = x;
        btn(glyph::tag, IdTagBtn, drop_open_ && drop_from_button_);
        x -= 8;

        // Segmented filter chips
        static constexpr const wchar_t* labels[] = {L"All", L"Images", L"Videos"};
        float widths[3];
        float total = 6;
        for (int i = 0; i < 3; ++i) {
            widths[i] = cv.measure(labels[i], Font::SmallSemi) + 26;
            total += widths[i];
        }
        x -= total;
        const D2D1_RECT_F seg = rect(x, cy - 17, total, 34);
        cv.fill_round(seg, 17, with_alpha(color::surface_highest, 0.85f));
        D2D1_RECT_F chip_rects[3]{};
        float cx = x + 3;
        for (int i = 0; i < 3; ++i) {
            chip_rects[i] = rect(cx, cy - 14, widths[i], 28);
            cx += widths[i];
        }
        const float ct = std::clamp(chip_anim_.value, 0.0f, 2.0f);
        for (int i = 0; i < 3; ++i) {
            const float act = std::max(0.0f, 1.0f - std::abs(ct - static_cast<float>(i)));
            const float ha = hover_.alpha(IdChip, i);
            if (act < 0.5f && ha > 0.002f) {
                cv.fill_round(chip_rects[i], 14, with_alpha(color::on_surface, 0.07f * ha));
            }
        }
        const int lo = std::clamp(static_cast<int>(std::floor(ct)), 0, 1);
        const float frac = ct - static_cast<float>(lo);
        cv.fill_round(lerp_rect(chip_rects[lo], chip_rects[lo + 1], frac), 14, color::primary_container);
        for (int i = 0; i < 3; ++i) {
            const float act = std::max(0.0f, 1.0f - std::abs(ct - static_cast<float>(i)));
            cv.text(labels[i], chip_rects[i], Font::SmallSemi,
                    lerp_color(color::on_surface_variant, color::on_primary_container, act), Align::Center);
            cv.hit(chip_rects[i], IdChip, i);
        }

        // Text field
        const D2D1_RECT_F fr = D2D1::RectF(52, 8, x - 14, HEADER_H - 8);
        cv.hit(fr, IdField);
        field_.draw(cv, D2D1::RectF(fr.left, fr.top, fr.right, fr.bottom), Font::Input, color::on_surface, color::outline);
        cv.hline(0, sz.width, HEADER_H, with_alpha(color::outline_variant, 0.7f));
    }

    [[nodiscard]] D2D1_COLOR_F status_dot_color() const {
        if (sh_.indexer_running) return color::amber;
        const auto st = sh_.engine.encoder().state();
        if (st == TextEncoder::State::Failed || !error_.empty() || (!sh_.engine.db_ready() && !sh_.test_mode)) return color::error;
        if (st == TextEncoder::State::Loading || st == TextEncoder::State::Exporting) return color::primary;
        return color::mint;
    }

    void draw_status(Canvas& cv, D2D1_SIZE_F sz) {
        const float y = HEADER_H + 1;
        float x = 18;
        auto piece = [&](std::wstring_view s, D2D1_COLOR_F c, Font f = Font::Caption) {
            if (s.empty()) return;
            const float w = cv.measure(s, f);
            cv.text(s, rect(x, y, w + 2, STATUS_H), f, c);
            x += w;
        };
        auto sep = [&] { piece(L"   •   ", color::outline_variant); };

        cv.icon(glyph::clock, rect(x - 2, y + 8, 14, 14), color::outline, Font::IconSmall);
        x += 16;
        if (sh_.indexer_running) {
            const auto& d = sh_.indexer;
            if (sh_.indexer_stopping) {
                piece(L"Stopping indexer…", color::amber, Font::SmallSemi);
            } else {
                piece(L"Indexing ", color::outline);
                if (d.phase == Phase::Embedding) piece(format_count(d.embedded) + L" / " + format_count(d.found), color::amber, Font::SmallSemi);
                else piece(phase_label(d.phase), color::amber, Font::SmallSemi);
            }
        } else {
            piece(L"Last indexed ", color::outline);
            piece(format_ago(sh_.index_state.last_index_unix), color::on_surface_variant, Font::SmallSemi);
        }
        if (sh_.db_count) {
            sep();
            piece(format_count(*sh_.db_count) + L" files", color::outline);
        }
        const auto enc = sh_.engine.encoder().state();
        if (enc == TextEncoder::State::Exporting) {
            sep();
            piece(L"Exporting text ONNX…", color::primary);
        } else if (enc == TextEncoder::State::Loading) {
            sep();
            piece(L"Loading model…", color::primary);
        }
        // A search fails with the model's own error while there is no model: show it once
        const std::wstring model_error = enc == TextEncoder::State::Failed ? sh_.engine.encoder().error() : std::wstring{};
        if (!model_error.empty()) {
            sep();
            piece(model_error, color::error);
        }
        if (!error_.empty() && error_ != model_error) {
            sep();
            piece(error_, color::error);
        } else if (last_ms_ > 0 && expanded()) {
            sep();
            wchar_t buf[48];
            swprintf_s(buf, L"%.0f ms", last_ms_);
            piece(buf, color::outline);
        }
        sep();
        piece(to_wide(sh_.settings.model_name), color::outline);
        if (sh_.hotkey_unavailable()) {
            sep();
            piece(format_hotkey(sh_.settings.hotkey_mods, sh_.settings.hotkey_vk) + L" is taken by another app", color::amber);
        }

        if (expanded() && !items_.empty()) {
            std::wstring right = format_count(items_.size()) + (has_more_ ? L"+" : L"") + L" results";
            if (selected_.size() > 1) right = format_count(selected_.size()) + L" selected  •  " + right;
            cv.text(right, D2D1::RectF(x + 12, y, sz.width - 18, y + STATUS_H), Font::Caption, color::outline, Align::Right);
        }
        if (expanded()) cv.hline(0, sz.width, HEADER_H + STATUS_H, with_alpha(color::outline_variant, 0.5f));
    }

    void draw_grid(Canvas& cv) {
        const Grid g = grid();
        cv.hit(g.view, IdGrid);
        cv.push_clip(g.view);

        if (items_.empty()) {
            const wchar_t* msg = (searching_ || debounce_pending_) ? L"" : (error_.empty() ? L"No matches" : L"");
            cv.text(msg, D2D1::RectF(g.view.left, g.view.top, g.view.right, g.view.top + 120), Font::Body, color::outline, Align::Center);
            if (searching_) draw_spinner(cv, {(g.view.left + g.view.right) / 2, g.view.top + 60});
        }

        const int first_row = std::max(0, static_cast<int>((scroll_ - PAD) / (g.card_h + GAP)));
        const int last_row = static_cast<int>((scroll_ + height(g.view)) / (g.card_h + GAP)) + 1;
        const UINT px = static_cast<UINT>(std::max(g.card_w, g.thumb_h * 1.6f) * target_.scale());
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
                    if (!sh_.thumbs.get(items_[i].pt.id)) sh_.thumbs.request(items_[i].pt.id, item_path(items_[i]), px);
                }
            }
        }

        if (has_more_ && !items_.empty()) {
            const float bottom = g.view.top + g.content_h - scroll_ - PAD - 20;
            if (loading_more_) draw_spinner(cv, {(g.view.left + g.view.right) / 2, bottom});
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
            const float ty = g.view.top + 4 + (track_h - thumb_h) * (scroll_ / ms);
            const float ha = dragging_scroll_ ? 1.0f : hover_.alpha(IdScrollbar);
            const D2D1_RECT_F tr = D2D1::RectF(g.view.right - lerp(7.0f, 9.0f, ha), ty, g.view.right - 3, ty + thumb_h);
            cv.fill_round(tr, 3, with_alpha(color::on_surface_variant, lerp(0.28f, 0.55f, ha)));
            cv.hit(D2D1::RectF(g.view.right - 12, g.view.top, g.view.right, g.view.bottom), IdScrollbar);
            scroll_thumb_ = tr;
        }
    }

    void draw_spinner(Canvas& cv, D2D1_POINT_2F c) {
        const float t = static_cast<float>(GetTickCount64() % 1200) / 1200.0f;
        for (int i = 0; i < 8; ++i) {
            const float a = (i / 8.0f + t) * 6.2831853f;
            const float alpha = 0.15f + 0.85f * (i / 8.0f);
            cv.fill_circle({c.x + std::cos(a) * 9, c.y + std::sin(a) * 9}, 2.0f, with_alpha(color::primary, alpha));
        }
        start_anim();
    }

    [[nodiscard]] float card_hover_alpha(int i) const noexcept {
        const float a = (hover_.id >= IdCard && hover_.id <= IdTagChip && hover_.index == i) ? hover_.in_anim.value : 0.0f;
        const float b = (hover_.prev_id >= IdCard && hover_.prev_id <= IdTagChip && hover_.prev_index == i) ? hover_.out_anim.value : 0.0f;
        return std::clamp(a + b, 0.0f, 1.0f);
    }

    void draw_card(Canvas& cv, const Grid& g, int i) {
        Item& it = items_[i];
        const D2D1_RECT_F r = card_rect(g, i);
        const float ha = card_hover_alpha(i);
        const bool sel = selected_.contains(it.pt.id);
        const bool focus = focus_ == i;

        cv.fill_round(r, 14, with_alpha(lerp_color(color::surface_low, color::surface_high, ha), 0.92f));
        if (sel) cv.stroke_round(r, 14, color::primary, 2.0f);
        else if (focus) cv.stroke_round(r, 14, with_alpha(color::primary, 0.6f), 1.0f);
        else cv.stroke_round(r, 14, with_alpha(color::outline_variant, lerp(0.45f, 0.9f, ha)));
        cv.hit(r, IdCard, i);

        // Thumbnail (cropped or letterboxed through a reusable bitmap brush so the corners stay rounded)
        const D2D1_RECT_F tr = rect(r.left + 6, r.top + 6, g.card_w - 12, g.thumb_h);
        if (ID2D1Bitmap1* bmp = sh_.thumbs.get(it.pt.id)) {
            const ThumbFit fit = parse_thumb_fit(sh_.settings.thumb_fit);
            if (fit == ThumbFit::Contain) cv.fill_round(tr, 10, color::surface_lowest);
            cv.fill_bitmap_round(bmp, tr, 10.0f, fit);
        } else {
            cv.fill_round(tr, 10, with_alpha(color::surface_highest, 0.6f));
            cv.icon(it.pt.p.k == 1 ? glyph::video : glyph::photo, tr, with_alpha(color::outline, 0.6f), Font::IconLarge);
        }

        if (it.pt.p.k == 1) {
            std::wstring dur = it.pt.p.d ? format_duration_ms(it.pt.p.d) : L"";
            const float dw = cv.measure(dur, Font::Caption);
            const D2D1_RECT_F b = rect(tr.right - dw - 34, tr.bottom - 30, dw + 26, 22);
            cv.fill_round(b, 11, hex(0x000000, 0.62f));
            cv.icon(glyph::play, rect(b.left + 6, b.top, 14, 22), hex(0xffffff), Font::IconSmall);
            cv.text(dur, rect(b.left + 21, b.top, dw + 2, 22), Font::Caption, hex(0xffffff));
        }
        if (ha > 0.002f) {
            const Canvas::ScopedOpacity op{cv, ha};
            auto overlay_btn = [&](float x, wchar_t g, int id) {
                const D2D1_RECT_F sb = rect(x, tr.top + 8, 28, 28);
                const float ba = hover_.alpha(id, i);
                cv.fill_circle({sb.left + 14, sb.top + 14}, 14, lerp_color(hex(0x000000, 0.55f), hex(0x2b2930, 0.95f), ba));
                cv.icon(g, sb, lerp_color(hex(0xffffff), color::primary, ba), Font::IconSmall);
                cv.hit(sb, id, i);
            };
            overlay_btn(tr.right - 36, glyph::similar, IdSimilar);
            overlay_btn(tr.right - 70, glyph::zoom, IdCardPreview);
        }

        float y = tr.bottom + 10.0f;
        const float lx = r.left + 12.0f, rw = g.card_w - 24.0f;
        const D2D1_RECT_F name_r = rect(lx, y, rw, 18.0f);
        cv.text(it.name, name_r, Font::BodySemi, color::on_surface);
        if (cv.measure(it.name, Font::BodySemi) > rw) cv.hit(name_r, IdCardName, i);
        y += 20.0f;
        const D2D1_RECT_F dir_r = rect(lx, y, rw, 16.0f);
        const std::wstring dir_disp = middle_truncate_dir(cv, it.dir, Font::MonoSmall, rw);
        cv.text(dir_disp, dir_r, Font::MonoSmall, color::outline);
        if (cv.measure(it.dir, Font::MonoSmall) > rw) cv.hit(dir_r, IdCardDir, i);
        y += 22.0f;

        // Tags (single line, +N overflow)
        float tx = lx;
        size_t shown = 0;
        for (const auto& t : it.tags_w) {
            const float w = cv.measure(t, Font::Caption) + 23.0f;
            if (tx + w > lx + rw - 34.0f && shown < it.tags_w.size()) break;
            draw_tag_chip(cv, tx, y, t, tag_color(it.pt.p.t[shown]));
            cv.hit(rect(tx, y, w, 20.0f), IdTagChip, i);
            tx += w + 5.0f;
            ++shown;
        }
        if (shown < it.tags_w.size()) {
            const std::wstring more = L"+" + std::to_wstring(it.tags_w.size() - shown);
            const float w = cv.measure(more, Font::Caption) + 12.0f;
            const D2D1_RECT_F mr = rect(tx, y, w, 20.0f);
            cv.fill_round(mr, 10.0f, with_alpha(color::on_surface, 0.08f));
            cv.text(more, mr, Font::Caption, color::on_surface_variant, Align::Center);
            cv.hit(mr, IdCardMoreTags, i);
        }
        y += 28.0f;
        cv.hline(lx, lx + rw, y, with_alpha(color::outline_variant, 0.6f));
        y += 8.0f;
        cv.text(it.meta, rect(lx, y, rw * 0.62f, 18.0f), Font::MonoSmall, color::on_surface_variant);
        cv.text(it.date, rect(lx + rw * 0.5f, y, rw * 0.5f, 18.0f), Font::Caption, color::outline, Align::Right);
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
        for (std::wstring_view p : {L"-tag:", L"!tag:", L"tag:"}) {
            if (tok.size() >= p.size() && _wcsnicmp(tok.data(), p.data(), p.size()) == 0) {
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
        std::stable_sort(drop_items_.begin(), drop_items_.end(), [&](const TagCount& a, const TagCount& b) {
            const bool pa = to_wide(a.v).starts_with(prefix), pb = to_wide(b.v).starts_with(prefix);
            return pa > pb;
        });
        if (drop_items_.size() > 8) drop_items_.resize(8);
        drop_open_ = !drop_items_.empty() || drop_from_button_;
        if (drop_items_.empty()) drop_sel_ = -1;
        else if (drop_sel_ < 0 || drop_sel_ >= static_cast<int>(drop_items_.size())) drop_sel_ = 0;
        apply_height();
    }

    void accept_dropdown(int index) {
        if (index < 0 || index >= static_cast<int>(drop_items_.size())) return;
        const std::wstring tag = to_wide(drop_items_[index].v);
        const std::wstring value = tag.contains(L' ') ? L"\"" + tag + L"\"" : tag;
        if (auto tok = tag_token()) {
            size_t end = field_.caret;
            while (end < field_.text.size() && !iswspace(field_.text[end])) ++end;
            field_.replace_range(tok->second, end - tok->second, value + L" ");
        } else {
            std::wstring t = trim(field_.text);
            if (!t.empty()) t += L' ';
            t += L"tag:" + value + L" ";
            field_.replace_all(t);
        }
        drop_open_ = false;
        drop_from_button_ = false;
        field_.focused = true;
        run_search(0);
    }

    void draw_dropdown(Canvas& cv) {
        const float x = 44.0f, y = HEADER_H - 4.0f, w = 300.0f;
        const float row = 32.0f;
        const float h = drop_items_.empty() ? 44.0f : 8.0f + row * static_cast<float>(drop_items_.size());
        const D2D1_RECT_F r = rect(x, y, w, h);
        cv.shadow(r, 12);
        cv.fill_round(r, 12, color::surface_high);
        cv.stroke_round(r, 12, color::outline_variant);
        cv.hit(r, IdDrop);
        if (drop_items_.empty()) {
            cv.text(L"No tags yet", inset(r, 14, 0), Font::Small, color::outline);
            return;
        }
        for (size_t i = 0; i < drop_items_.size(); ++i) {
            const auto& t = drop_items_[i];
            const D2D1_RECT_F rr = rect(x + 4, y + 4 + row * i, w - 8, row);
            if (static_cast<int>(i) == drop_sel_) cv.fill_round(rr, 8, with_alpha(color::primary, 0.14f));
            const D2D1_COLOR_F c = tag_color(t.v);
            cv.fill_circle({rr.left + 14, (rr.top + rr.bottom) / 2}, 4, c);
            cv.text(to_wide(t.v), D2D1::RectF(rr.left + 28, rr.top, rr.right - 60, rr.bottom), Font::Body, color::on_surface);
            cv.text(format_count(t.c), D2D1::RectF(rr.right - 70, rr.top, rr.right - 12, rr.bottom), Font::Caption, color::outline, Align::Right);
            cv.hit(rr, IdDropItem, static_cast<int>(i));
        }
    }

    struct HelpRow {
        const wchar_t* syntax;
        const wchar_t* desc;
        const wchar_t* insert;
    };
    static constexpr HelpRow HELP_ROWS[] = {
        {L"free text", L"Semantic search by meaning (CLIP)", L""},
        {L"name:cat", L"File name contains prefix tokens", L"name:"},
        {L"tag:fav", L"Require tag (\"quotes\" for spaces)", L"tag:"},
        {L"-tag:wip", L"Exclude tag (or !tag:wip)", L"-tag:"},
        {L"dir:C:\\Photos", L"Search inside folder (or in:, bare path)", L"dir:"},
        {L"similar:path", L"Visually similar to an indexed file", L"similar:"},
        {L"type:image", L"Filter media type (image or video)", L"type:image "},
    };

    void draw_help(Canvas& cv, D2D1_SIZE_F sz) {
        const float x = std::clamp(help_btn_x_ + 36.0f - HELP_W, 14.0f, std::max(14.0f, sz.width - HELP_W - 14.0f));
        const float y = HEADER_H - 4.0f;
        const D2D1_RECT_F r = rect(x, y, HELP_W, HELP_H);
        cv.shadow(r, 12, 12, 0.35f);
        cv.fill_round(r, 12, color::surface_high);
        cv.stroke_round(r, 12, color::outline_variant);
        cv.hit(rect(x, HEADER_H - 18.0f, HELP_W, HELP_H + 14.0f), IdHelp);

        cv.text(L"SEARCH SYNTAX  ·  CLICK TO INSERT", rect(x + 14.0f, y + 8.0f, HELP_W - 28.0f, 22.0f), Font::Label, color::primary);
        float ry = y + 34.0f;
        for (int i = 0; i < static_cast<int>(std::size(HELP_ROWS)); ++i) {
            const D2D1_RECT_F rr = rect(x + 6.0f, ry, HELP_W - 12.0f, 28.0f);
            if (const float ha = hover_.alpha(IdHelpItem, i); ha > 0.002f && HELP_ROWS[i].insert[0] != 0) {
                cv.fill_round(rr, 8.0f, with_alpha(color::primary, 0.12f * ha));
            }
            const D2D1_RECT_F badge = rect(rr.left + 6.0f, rr.top + 3.0f, 114.0f, 22.0f);
            cv.fill_round(badge, 6.0f, with_alpha(color::surface_highest, 0.9f));
            cv.text(HELP_ROWS[i].syntax, inset(badge, 8.0f, 0.0f), Font::MonoSmall, color::primary);
            cv.text(HELP_ROWS[i].desc, D2D1::RectF(rr.left + 130.0f, rr.top, rr.right - 8.0f, rr.bottom), Font::Small, color::on_surface_variant);
            if (HELP_ROWS[i].insert[0] != 0) cv.hit(rr, IdHelpItem, i);
            ry += 28.0f;
        }
    }

    void insert_help_syntax(int index) {
        if (index < 0 || index >= static_cast<int>(std::size(HELP_ROWS))) return;
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
        if (ins.ends_with(L' ')) run_search(0);
        else apply_height();
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

    void close_popups() {
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
        size_t with = 0, total = 0;
        for (const auto& it : items_) {
            if (!selected_.contains(it.pt.id)) continue;
            ++total;
            if (std::find(it.pt.p.t.begin(), it.pt.p.t.end(), tag) != it.pt.p.t.end()) ++with;
        }
        return with == 0 ? 0 : with == total ? 2 : 1;
    }

    void set_tag(const std::string& tag, bool on) {
        if (tag.empty()) return;
        struct Change {
            uint64_t id;
            std::vector<std::string> tags;
        };
        std::vector<Change> changes;
        for (auto& it : items_) {
            if (!selected_.contains(it.pt.id)) continue;
            auto& t = it.pt.p.t;
            const auto pos = std::find(t.begin(), t.end(), tag);
            if (on && pos == t.end()) t.push_back(tag);
            else if (!on && pos != t.end()) t.erase(pos);
            else continue;
            it.tags_w.clear();
            for (const auto& s : t) it.tags_w.push_back(to_wide(s));
            changes.push_back({it.pt.id, t});
        }
        if (changes.empty()) return;
        if (on && std::find(menu_tags_.begin(), menu_tags_.end(), tag) == menu_tags_.end()) menu_tags_.insert(menu_tags_.begin(), tag);
        sh_.engine.post([this, changes = std::move(changes)](Db& db) {
            const std::string coll = sh_.engine.collection();
            for (const auto& c : changes) db.set_tags(coll, std::span{&c.id, 1}, c.tags);
            sh_.refresh_tags();
        });
        invalidate();
    }

    [[nodiscard]] D2D1_RECT_F menu_rect() const {
        const D2D1_SIZE_F sz = target_.size();
        const float h = static_cast<float>(ActCount) * 36.0f + 16 + 30 + std::min<size_t>(menu_tags_.size(), 10) * 32.0f + 48;
        float x = std::min(menu_at_.x, sz.width - MENU_W - 8);
        float y = menu_at_.y;
        if (y + h > sz.height - 8) y = std::max(8.0f, sz.height - 8 - h);
        return rect(std::max(8.0f, x), y, MENU_W, h);
    }

    void draw_menu(Canvas& cv) {
        const D2D1_RECT_F r = menu_rect();
        cv.shadow(r, 14, 14, 0.4f);
        cv.fill_round(r, 14, color::surface_high);
        cv.stroke_round(r, 14, color::outline_variant);
        cv.hit(r, IdMenu);

        struct Act {
            wchar_t g;
            const wchar_t* label;
            const wchar_t* key;
        };
        static constexpr Act acts[ActCount] = {
            {glyph::open, L"Open", L"Enter"},
            {glyph::zoom, L"Preview", L"Space"},
            {glyph::folder, L"Show in Explorer", L"Ctrl+E"},
            {glyph::similar, L"Find similar", L"Ctrl+S"},
            {glyph::copy, L"Copy media", L"Ctrl+C"},
            {glyph::link, L"Copy path", L"Ctrl+Shift+C"},
        };
        float y = r.top + 6;
        for (int i = 0; i < ActCount; ++i) {
            const D2D1_RECT_F rr = rect(r.left + 6, y, MENU_W - 12, 36);
            if (const float ha = hover_.alpha(IdMenuItem, i); ha > 0.002f) cv.fill_round(rr, 8, with_alpha(color::on_surface, 0.08f * ha));
            cv.icon(acts[i].g, rect(rr.left + 8, rr.top, 20, 36), color::on_surface_variant);
            cv.text(acts[i].label, D2D1::RectF(rr.left + 38, rr.top, rr.right - 86, rr.bottom), Font::Body, color::on_surface);
            cv.text(acts[i].key, D2D1::RectF(rr.right - 96, rr.top, rr.right - 10, rr.bottom), Font::Caption, color::outline, Align::Right);
            cv.hit(rr, IdMenuItem, i);
            y += 36;
        }
        y += 4;
        cv.hline(r.left + 12, r.right - 12, y, color::outline_variant);
        y += 6;
        std::wstring label = L"TAGS";
        if (selected_.size() > 1) label += L"  ·  " + std::to_wstring(selected_.size()) + L" ITEMS";
        cv.text(label, rect(r.left + 16, y, MENU_W - 32, 24), Font::Label, color::primary);
        y += 26;
        for (size_t i = 0; i < menu_tags_.size() && i < 10; ++i) {
            const auto& t = menu_tags_[i];
            const D2D1_RECT_F rr = rect(r.left + 6, y, MENU_W - 12, 32);
            if (const float ha = hover_.alpha(IdMenuTag, static_cast<int>(i)); ha > 0.002f) cv.fill_round(rr, 8, with_alpha(color::on_surface, 0.08f * ha));
            const int st = tag_state(t);
            const D2D1_RECT_F box = rect(rr.left + 10, rr.top + 8, 16, 16);
            if (st == 2) {
                cv.fill_round(box, 4, color::primary);
                cv.icon(glyph::check, box, color::on_primary, Font::IconSmall);
            } else {
                cv.stroke_round(box, 4, st == 1 ? color::primary : color::outline, 1.5f);
                if (st == 1) cv.fill(rect(box.left + 4, box.top + 7, 8, 2), color::primary);
            }
            const D2D1_COLOR_F c = tag_color(t);
            cv.fill_circle({rr.left + 40, (rr.top + rr.bottom) / 2}, 3.5f, c);
            cv.text(to_wide(t), D2D1::RectF(rr.left + 50, rr.top, rr.right - 10, rr.bottom), Font::Body, color::on_surface);
            cv.hit(rr, IdMenuTag, static_cast<int>(i));
            y += 32;
        }
        const D2D1_RECT_F fr = rect(r.left + 10, y + 6, MENU_W - 20, 34);
        cv.fill_round(fr, 8, color::surface_highest);
        if (menu_field_.focused) cv.stroke_round(fr, 8, color::primary, 1.5f);
        cv.icon(glyph::add, rect(fr.left + 4, fr.top, 24, 34), color::outline, Font::IconSmall);
        menu_field_.draw(cv, D2D1::RectF(fr.left + 30, fr.top, fr.right - 10, fr.bottom), Font::Body, color::on_surface, color::outline);
        cv.hit(fr, IdMenuField);
    }

    void menu_action(int a) {
        std::vector<const Item*> sel;
        for (const auto& it : items_) {
            if (selected_.contains(it.pt.id)) sel.push_back(&it);
        }
        const bool have_index = menu_index_ >= 0 && menu_index_ < static_cast<int>(items_.size());
        if (sel.empty() && have_index) sel.push_back(&items_[menu_index_]);
        if (sel.empty()) return;
        // Single-item actions follow the item the menu was opened on (or the keyboard focus)
        const Item& primary = have_index && selected_.contains(items_[menu_index_].pt.id) ? items_[menu_index_] : *sel.front();
        close_popups();
        switch (a) {
            case ActOpen:
                for (size_t i = 0; i < sel.size() && i < 8; ++i) open_file(item_path(*sel[i]));
                break;
            case ActPreview: open_preview(static_cast<int>(&primary - items_.data())); return;
            case ActReveal: reveal_in_explorer(item_path(primary)); break;
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
                set_clipboard_text(hwnd_, all);
                break;
            }
        }
        invalidate();
    }

    void find_similar(const Item& it) {
        field_.replace_all(L"similar:" + item_path(it));
        field_.focused = true;
        run_search(0);
    }

    /// Copies the files themselves; a single still image also goes on as pixels so editors can paste it.
    /// Only the decode runs on a worker; the newest copy wins and reaches the clipboard from the UI thread.
    void copy_media(const std::vector<const Item*>& sel) {
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
            ulp::win::COMWrapper<false> com{COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE};
            auto payload = std::make_shared<ClipboardPayload>();
            if (!stop.stop_requested()) {
                std::optional<Raster> image;
                std::optional<std::wstring> png;
                if (decode) {
                    image = decode_image(paths[0], 0, false);
                    if (image && static_cast<uint64_t>(image->w) * image->h > COPY_MAX_PIXELS) image.reset();
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
        if (SUCCEEDED(hr)) OleFlushClipboard();
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

    static constexpr float PV_TOP = 56.0f;
    static constexpr float PV_BOTTOM = 44.0f;
    static constexpr float PV_CONTROLS_H = 42.0f;

    [[nodiscard]] bool preview_valid() const noexcept { return preview_ >= 0 && preview_ < static_cast<int>(items_.size()); }

    [[nodiscard]] bool preview_has_controls() const noexcept {
        if (!preview_valid()) return false;
        const Item& it = items_[preview_];
        if (it.pt.p.k == 1) return !video_.failed();
        if (is_gif_path(it.name)) return gif_.has_animation() || (gif_.active() && !gif_.settled_static());
        return false;
    }

    [[nodiscard]] static D2D1_RECT_F preview_media_area(D2D1_SIZE_F sz, bool has_controls) noexcept {
        const float bottom = std::max(PV_TOP + 48.0f, sz.height - PV_BOTTOM - (has_controls ? PV_CONTROLS_H : 0.0f));
        return D2D1::RectF(16.0f, PV_TOP, std::max(32.0f, sz.width - 16.0f), bottom);
    }

    [[nodiscard]] D2D1_RECT_F preview_dest_dims(uint32_t src_w, uint32_t src_h, uint32_t meta_w, uint32_t meta_h, D2D1_RECT_F area) const {
        const FitBox b = compute_preview_box(src_w, src_h, meta_w, meta_h, target_.scale(),
                                             FitBox{.left = area.left, .top = area.top, .width = width(area), .height = height(area)});
        return rect(b.left, b.top, b.width, b.height);
    }

    [[nodiscard]] D2D1_RECT_F preview_dest(const Item& it, ID2D1Bitmap* bmp, D2D1_RECT_F area) const {
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
                    const D2D1_RECT_F area = preview_media_area(target_.size(), preview_has_controls());
                    const D2D1_RECT_F dest = preview_dest_dims(nw, nh, nw, nh, area);
                    const auto px_w = static_cast<uint32_t>(std::max(1.0f, std::round(width(dest) * target_.scale())));
                    const auto px_h = static_cast<uint32_t>(std::max(1.0f, std::round(height(dest) * target_.scale())));
                    if (video_.set_output_size(px_w, px_h) &&
                        video_.tick(sh_.gfx.d3d(), target_.dc()) == MediaTickStatus::DeviceLost) {
                        recover();
                        return false;
                    }
                }
            }
        } else if (gif_.active() && gif_.has_frame() && gif_.sync_bitmap(target_.dc()) == MediaTickStatus::DeviceLost) {
            recover();
            return false;
        }
        return true;
    }

    void sync_preview_timers() {
        if (!hwnd_) return;
        if (!visible() || !preview_valid()) {
            KillTimer(hwnd_, TIMER_GIF);
            KillTimer(hwnd_, TIMER_VIDEO);
            return;
        }
        const Item& it = items_[preview_];
        if (it.pt.p.k == 1) {
            KillTimer(hwnd_, TIMER_GIF);
            if (video_.active() && !video_.failed() && video_.is_playing()) {
                SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
            } else {
                KillTimer(hwnd_, TIMER_VIDEO);
            }
        } else {
            KillTimer(hwnd_, TIMER_VIDEO);
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

    void invalidate_preview_media() {
        if (!visible()) return;
        if (pv_media_rect_.right <= pv_media_rect_.left || pv_media_rect_.bottom <= pv_media_rect_.top) {
            invalidate();
            return;
        }
        pending_dirty_.push_back(D2D1::RectF(pv_media_rect_.left - 2.0f, pv_media_rect_.top - 2.0f,
                                             pv_media_rect_.right + 2.0f, pv_media_rect_.bottom + 2.0f));
        if (pv_ctrl_rect_.right > pv_ctrl_rect_.left && pv_ctrl_rect_.bottom > pv_ctrl_rect_.top) {
            pending_dirty_.push_back(D2D1::RectF(pv_ctrl_rect_.left - 4.0f, pv_ctrl_rect_.top - 4.0f,
                                                 pv_ctrl_rect_.right + 4.0f, pv_ctrl_rect_.bottom + 4.0f));
        }
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void on_gif_worker() {
        if (!preview_valid() || items_[preview_].pt.p.k == 1) return;
        const bool had_anim = gif_.has_animation();
        const bool had_frame = gif_.has_frame();
        if (gif_.process_worker_signal()) {
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
        sync_preview_timers();
        if (!preview_valid() || items_[preview_].pt.p.k != 1) return;
        if (target_.ready()) {
            const MediaTickStatus st = video_.tick(sh_.gfx.d3d(), target_.dc());
            if (st == MediaTickStatus::DeviceLost) {
                recover();
                return;
            }
            if (had_frame != video_.has_frame() || had_failed != video_.failed()) {
                invalidate();
            } else if (st == MediaTickStatus::FrameReady || changed) {
                last_scrubber_redraw_ = std::chrono::steady_clock::now();
                invalidate_preview_media();
            }
        } else if (changed) {
            invalidate();
        }
    }

    void on_video_timer() {
        if (!visible() || !preview_valid() || items_[preview_].pt.p.k != 1 || !video_.active() || !video_.is_playing()) {
            KillTimer(hwnd_, TIMER_VIDEO);
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
        } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scrubber_redraw_).count() >= 200) {
            last_scrubber_redraw_ = now;
            invalidate_preview_controls();
        }
    }

    void open_preview(int index) {
        if (index < 0 || index >= static_cast<int>(items_.size())) return;
        close_popups();
        clear_hover();
        preview_wheel_ = 0;
        preview_initial_index_ = index;
        preview_initial_scroll_ = scroll_;
        preview_go(index);
    }

    void close_preview(bool restore_focus = true) {
        if (preview_ < 0) return;
        const int last = preview_;
        preview_ = -1;
        dragging_seek_ = false;
        pv_media_rect_ = {};
        pv_ctrl_rect_ = {};
        pv_seek_rect_ = {};
        KillTimer(hwnd_, TIMER_GIF);
        KillTimer(hwnd_, TIMER_VIDEO);
        gif_.close();
        video_.close();
        sh_.previews.clear();
        clear_hover();
        if (restore_focus && last < static_cast<int>(items_.size())) {
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
        dragging_seek_ = false;
        pv_media_rect_ = {};
        pv_ctrl_rect_ = {};
        pv_seek_rect_ = {};
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
    [[nodiscard]] UINT preview_px() const {
        MONITORINFO mi{.cbSize = sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        return static_cast<UINT>(std::max({mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, 256L}));
    }

    void request_preview() {
        if (!preview_valid()) return;
        const UINT px = preview_px();
        const auto want = [&](int k) {
            if (k >= 0 && k < static_cast<int>(items_.size())) {
                sh_.previews.request(items_[static_cast<size_t>(k)].pt.id, item_path(items_[static_cast<size_t>(k)]), px);
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
            video_.set_muted(sh_.test_mode || video_muted_);
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

    void draw_preview_controls(Canvas& cv, D2D1_SIZE_F sz, D2D1_RECT_F area, D2D1_RECT_F dest,
                               const Item& it, bool is_video, bool is_gif) {
        const bool is_video_ctrl = is_video && video_.active() && !video_.failed();
        const bool is_gif_ctrl = is_gif && gif_.has_animation();
        if (!is_video_ctrl && !is_gif_ctrl) {
            pv_ctrl_rect_ = {};
            pv_seek_rect_ = {};
            return;
        }

        const float max_bar_w = std::max(280.0f, sz.width - 140.0f);
        const float cw = std::clamp(std::max(320.0f, width(dest)), 280.0f, max_bar_w);
        const float cx = (sz.width - cw) * 0.5f;
        const D2D1_RECT_F ctrl = rect(cx, area.bottom + 4.0f, cw, 34.0f);
        pv_ctrl_rect_ = ctrl;

        cv.fill_round(ctrl, 17.0f, with_alpha(color::surface_low, 0.92f));
        cv.stroke_round(ctrl, 17.0f, color::outline_variant, 1.0f);
        cv.hit(ctrl, IdPvBar);

        float lx = ctrl.left + 5.0f;
        float rx = ctrl.right - 5.0f;
        const bool playing = is_video_ctrl ? video_.is_playing() : gif_.playing();
        const D2D1_RECT_F play_r = rect(lx, ctrl.top + 3.0f, 28.0f, 28.0f);
        icon_button(cv, play_r, playing ? glyph::pause : glyph::play, IdPvPlay);
        lx += 32.0f;

        float ratio = 0.0f;
        if (is_video_ctrl) {
            const D2D1_RECT_F mute_r = rect(rx - 28.0f, ctrl.top + 3.0f, 28.0f, 28.0f);
            icon_button(cv, mute_r, video_.is_muted() ? glyph::mute : glyph::volume, IdPvMute, video_.is_muted());
            rx -= 32.0f;

            double dur_s = video_.duration();
            if (dur_s <= 0.0 && it.pt.p.d > 0) dur_s = static_cast<double>(it.pt.p.d) / 1000.0;
            const double cur_s = dragging_seek_ ? static_cast<double>(seek_drag_ratio_) * dur_s : video_.current_time();
            ratio = dragging_seek_ ? seek_drag_ratio_
                                   : (dur_s > 0.0 ? static_cast<float>(std::clamp(cur_s / dur_s, 0.0, 1.0)) : 0.0f);

            const std::wstring cur_str = format_duration_ms(static_cast<uint64_t>(std::max(0.0, cur_s * 1000.0)));
            const std::wstring dur_str = dur_s > 0.0
                ? format_duration_ms(static_cast<uint64_t>(dur_s * 1000.0))
                : L"--:--";
            const float cur_w = std::max(34.0f, cv.measure(cur_str, Font::MonoSmall) + 6.0f);
            const float dur_w = std::max(34.0f, cv.measure(dur_str, Font::MonoSmall) + 6.0f);
            cv.text(cur_str, rect(lx, ctrl.top, cur_w, height(ctrl)), Font::MonoSmall, color::on_surface_variant, Align::Center);
            lx += cur_w + 4.0f;
            cv.text(dur_str, rect(rx - dur_w, ctrl.top, dur_w, height(ctrl)), Font::MonoSmall, color::outline, Align::Center);
            rx -= dur_w + 4.0f;
        } else {
            const D2D1_RECT_F badge_r = rect(lx + 2.0f, ctrl.top + 8.0f, 30.0f, 18.0f);
            cv.fill_round(badge_r, 9.0f, with_alpha(color::surface_highest, 0.95f));
            cv.text(L"GIF", badge_r, Font::Label, color::primary, Align::Center);
            lx += 36.0f;

            const size_t total = gif_.frame_count();
            const size_t cur = dragging_seek_
                ? static_cast<size_t>(std::lround(seek_drag_ratio_ * static_cast<float>(total > 0 ? total - 1 : 0)))
                : gif_.current_frame();
            ratio = dragging_seek_ ? seek_drag_ratio_
                                   : (total > 1 ? static_cast<float>(cur) / static_cast<float>(total - 1) : 0.0f);

            const std::wstring fstr = std::to_wstring(cur + 1) + L"/" + std::to_wstring(total);
            const float fw = std::max(38.0f, cv.measure(fstr, Font::MonoSmall) + 8.0f);
            cv.text(fstr, rect(rx - fw, ctrl.top, fw, height(ctrl)), Font::MonoSmall, color::outline, Align::Center);
            rx -= fw + 4.0f;
        }

        const float sx0 = lx + 6.0f;
        const float sx1 = rx - 6.0f;
        if (sx1 > sx0 + 16.0f) {
            pv_seek_rect_ = D2D1::RectF(sx0, ctrl.top, sx1, ctrl.bottom);
            const float cy = (ctrl.top + ctrl.bottom) * 0.5f;
            const D2D1_RECT_F track = D2D1::RectF(sx0, cy - 2.0f, sx1, cy + 2.0f);
            cv.fill_round(track, 2.0f, color::surface_highest);

            const float thumb_x = sx0 + ((sx1 - sx0) * std::clamp(ratio, 0.0f, 1.0f));
            if (thumb_x > sx0 + 0.5f) {
                cv.fill_round(D2D1::RectF(sx0, cy - 2.0f, thumb_x, cy + 2.0f), 2.0f, color::primary);
            }
            const float ha = hover_.alpha(IdPvSeek);
            const float rad = (dragging_seek_ || ha > 0.05f) ? 5.5f : 4.2f;
            cv.fill_circle({.x = thumb_x, .y = cy}, rad, lerp_color(color::on_surface, color::primary, dragging_seek_ ? 1.0f : ha * 0.5f));
            cv.hit(D2D1::RectF(sx0 - 6.0f, ctrl.top, sx1 + 6.0f, ctrl.bottom), IdPvSeek);
        } else {
            pv_seek_rect_ = {};
        }
    }

    void draw_preview(Canvas& cv, D2D1_SIZE_F sz) {
        if (!preview_valid()) {
            preview_ = -1;
            return;
        }
        const Item& it = items_[preview_];
        const int count = static_cast<int>(items_.size());
        cv.fill(D2D1::RectF(0, 0, sz.width, sz.height), with_alpha(color::surface_lowest, 0.94f));
        // Backdrop below the title bar closes the viewer; the bar itself still drags the window
        cv.hit(D2D1::RectF(0, PV_TOP, sz.width, sz.height), IdPreview);

        const bool is_video = (it.pt.p.k == 1);
        const bool is_gif_ready = !is_video && gif_.has_animation() && gif_.has_frame() && gif_.bitmap();
        const D2D1_RECT_F area = preview_media_area(sz, preview_has_controls());

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
                cv.bitmap(poster, dest);
                cv.hit(dest, IdPvImage);
            } else {
                cv.icon(glyph::video, rect(area.left, ((area.top + area.bottom) / 2) - 20, width(area), 40), color::on_surface_variant, Font::IconLarge);
            }
        } else if (is_gif_ready) {
            ID2D1Bitmap1* gbmp = gif_.bitmap();
            const D2D1_SIZE_U gps = gbmp->GetPixelSize();
            const uint32_t mw = gif_.native_width() > 0 ? gif_.native_width() : it.pt.p.w;
            const uint32_t mh = gif_.native_height() > 0 ? gif_.native_height() : it.pt.p.h;
            dest = preview_dest_dims(gps.width, gps.height, mw, mh, area);
            cv.shadow(dest, 4, 14, 0.35f);
            cv.bitmap(gbmp, dest);
            cv.hit(dest, IdPvImage);
        } else {
            ID2D1Bitmap1* full = sh_.previews.get(it.pt.id);
            ID2D1Bitmap1* bmp = full ? full : sh_.thumbs.get(it.pt.id);
            const bool loading = is_video ? video_.is_loading() : (!full && !sh_.previews.failed(it.pt.id));
            if (bmp) {
                dest = preview_dest(it, bmp, area);
                cv.shadow(dest, 4, 14, 0.35f);
                cv.bitmap(bmp, dest);
                cv.hit(dest, IdPvImage);
                if (loading) draw_spinner(cv, {.x = dest.right - 20, .y = dest.bottom - 20});
            } else if (loading) {
                draw_spinner(cv, {.x = (area.left + area.right) / 2, .y = (area.top + area.bottom) / 2});
            } else {
                cv.icon(is_video ? glyph::video : glyph::photo, rect(area.left, ((area.top + area.bottom) / 2) - 40, width(area), 40), color::outline, Font::IconLarge);
                cv.text(L"No preview available", rect(area.left, (area.top + area.bottom) / 2, width(area), 24), Font::Body, color::outline, Align::Center);
            }
            if (is_video && video_.failed()) {
                const D2D1_POINT_2F c{.x = (dest.left + dest.right) / 2, .y = (dest.top + dest.bottom) / 2};
                const float ha = hover_.alpha(IdPvPlay);
                cv.fill_circle(c, 30, hex(0x000000, lerp(0.55f, 0.75f, ha)));
                cv.icon(glyph::play, rect(c.x - 28, c.y - 30, 60, 60), lerp_color(hex(0xffffff), color::primary, ha), Font::IconLarge);
                cv.hit(rect(c.x - 30, c.y - 30, 60, 60), IdPvPlay);
            }
        }
        pv_media_rect_ = dest;

        draw_preview_controls(cv, sz, area, dest, it, is_video, is_gif_ready);

        // Previous / next
        auto arrow = [&](bool next) {
            const bool can = next ? preview_ + 1 < count : preview_ > 0;
            if (!can) return;
            const float cx = next ? area.right - 28 : area.left + 28, cy = (area.top + area.bottom) / 2;
            const int id = next ? IdPvNext : IdPvPrev;
            const float ha = hover_.alpha(id);
            cv.fill_circle({cx, cy}, 22, lerp_color(hex(0x000000, 0.45f), with_alpha(color::surface_highest, 0.96f), ha));
            cv.icon(next ? glyph::chevron_right : glyph::chevron_left, rect(cx - 22, cy - 22, 44, 44), lerp_color(hex(0xffffff, 0.9f), color::on_surface, ha));
            cv.hit(rect(cx - 28, cy - 44, 56, 88), id);
        };
        arrow(false);
        arrow(true);

        // Title bar: name and folder, position, actions
        float x = sz.width - 12;
        const float cy = PV_TOP / 2;
        auto btn = [&](wchar_t g, int id) {
            x -= 36;
            icon_button(cv, rect(x, cy - 18, 36, 36), g, id);
        };
        btn(glyph::close, IdPvClose);
        x -= 8;
        cv.vline(x, cy - 12, cy + 12, color::outline_variant);
        x -= 4;
        btn(glyph::copy, IdPvCopy);
        btn(glyph::folder, IdPvReveal);
        btn(glyph::open, IdPvOpen);
        const std::wstring pos = format_count(static_cast<uint64_t>(preview_) + 1) + L" / " + format_count(items_.size()) + (has_more_ ? L"+" : L"");
        const float pw = cv.measure(pos, Font::SmallSemi) + 24;
        x -= pw + 6;
        const D2D1_RECT_F pr = rect(x, cy - 13, pw, 26);
        cv.fill_round(pr, 13, with_alpha(color::surface_highest, 0.85f));
        cv.text(pos, pr, Font::SmallSemi, color::on_surface_variant, Align::Center);
        const float path_w = std::max(40.0f, x - 34.0f);
        const std::wstring dir_disp = middle_truncate_dir(cv, it.dir, Font::MonoSmall, path_w);
        cv.text(it.name, D2D1::RectF(18, 9, x - 16, 29), Font::BodySemi, color::on_surface);
        cv.text(dir_disp, D2D1::RectF(18, 29, x - 16, 46), Font::MonoSmall, color::outline);
        if (cv.measure(it.name, Font::BodySemi) > path_w || cv.measure(it.dir, Font::MonoSmall) > path_w) {
            cv.hit(D2D1::RectF(18, 9, x - 16, 46), IdPvPath, preview_);
        }

        // Footer: size, date, duration
        std::wstring meta = it.meta;
        if (is_video && it.pt.p.d) meta = format_duration_ms(it.pt.p.d) + L"  •  " + meta;
        if (!it.date.empty()) meta += L"  •  " + it.date;
        cv.text(meta, D2D1::RectF(18, sz.height - PV_BOTTOM, sz.width - 18, sz.height), Font::Caption, color::outline, Align::Center);
    }

    void preview_seek_to_x(float x) {
        if (!preview_valid() || pv_seek_rect_.right <= pv_seek_rect_.left) return;
        const float ratio = std::clamp((x - pv_seek_rect_.left) / width(pv_seek_rect_), 0.0f, 1.0f);
        seek_drag_ratio_ = ratio;
        if (items_[preview_].pt.p.k == 1) {
            const double dur = video_.duration();
            if (dur > 0.0) video_.seek(static_cast<double>(ratio) * dur);
        } else if (gif_.has_animation()) {
            gif_.seek_fraction(ratio);
            sync_preview_timers();
        }
    }

    void preview_toggle_play() {
        if (!preview_valid()) return;
        if (items_[preview_].pt.p.k == 1 && !video_.failed()) {
            video_.toggle_play();
            sync_preview_timers();
            invalidate_preview_controls();
        } else if (gif_.has_animation()) {
            gif_.toggle_play();
            sync_preview_timers();
            invalidate_preview_controls();
        }
    }

    void preview_click(int id, D2D1_POINT_2F p, bool dbl) {
        const std::wstring path = item_path(items_[preview_]);
        switch (id) {
            case IdPvClose: close_preview(); return;
            case IdPvPrev: preview_step(-1); return;
            case IdPvNext: preview_step(1); return;
            case IdPvOpen: open_file(path); return;
            case IdPvPlay:
                if (items_[preview_].pt.p.k == 1 && video_.failed()) {
                    open_file(path);
                } else {
                    preview_toggle_play();
                }
                return;
            case IdPvMute:
                if (items_[preview_].pt.p.k == 1 && !video_.failed()) {
                    video_muted_ = !video_muted_;
                    video_.set_muted(sh_.test_mode || video_muted_);
                    invalidate_preview_controls();
                }
                return;
            case IdPvSeek:
                dragging_seek_ = true;
                SetCapture(hwnd_);
                preview_seek_to_x(p.x);
                invalidate_preview_media();
                return;
            case IdPvBar: return;
            case IdPvReveal: reveal_in_explorer(path); return;
            case IdPvCopy: copy_media({&items_[preview_]}); return;
            case IdPvImage:
                if (dbl) {
                    open_file(path);
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
            case VK_RETURN: open_file(item_path(it)); return true;
            case 'K':
            case 'P':
            case VK_MEDIA_PLAY_PAUSE:
                preview_toggle_play();
                return true;
            case 'M':
                if (it.pt.p.k == 1 && !video_.failed()) {
                    video_muted_ = !video_muted_;
                    video_.set_muted(sh_.test_mode || video_muted_);
                    invalidate_preview_controls();
                }
                return true;
            case 'J':
            case VK_OEM_COMMA:
                if (it.pt.p.k == 1 && !video_.failed()) {
                    video_.seek_relative(vk == 'J' ? -5.0 : -0.05);
                    invalidate_preview_media();
                } else if (gif_.has_animation()) {
                    gif_.step_frame(-1);
                    sync_preview_timers();
                    invalidate_preview_media();
                }
                return true;
            case 'L':
            case VK_OEM_PERIOD:
                if (it.pt.p.k == 1 && !video_.failed()) {
                    video_.seek_relative(vk == 'L' ? 5.0 : 0.05);
                    invalidate_preview_media();
                } else if (gif_.has_animation()) {
                    gif_.step_frame(1);
                    sync_preview_timers();
                    invalidate_preview_media();
                }
                return true;
            default: break;
        }
        if (ctrl && vk == 'C') {
            if (shift) {
                set_clipboard_text(hwnd_, item_path(it));
            } else {
                copy_media({&it});
            }
        } else if (ctrl && vk == 'E') {
            reveal_in_explorer(item_path(it));
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

    void begin_drag_detect(int index) {
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
        if (drag_index_ < 0 || drag_index_ >= static_cast<int>(items_.size())) return;
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
        auto async_drop = drag_media(hwnd_, paths);
        dragging_out_ = false;
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

    [[nodiscard]] D2D1_POINT_2F to_dip(LPARAM lp) const {
        const float s = target_.scale();
        return {GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s};
    }

    void caret_reset() {
        field_.caret_on = true;
        menu_field_.caret_on = true;
        SetTimer(hwnd_, TIMER_CARET, GetCaretBlinkTime(), nullptr);
    }

    void on_mouse_move(D2D1_POINT_2F p) {
        if (fading_out_) return;
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        if (drag_pending_) {
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                end_drag_detect();
                ReleaseCapture();
            } else {
                POINT c;
                GetCursorPos(&c);
                if (std::abs(c.x - drag_origin_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(c.y - drag_origin_.y) > GetSystemMetrics(SM_CYDRAG)) {
                    start_drag();
                    return;
                }
            }
        }
        if (dragging_seek_) {
            if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                dragging_seek_ = false;
                ReleaseCapture();
            } else {
                preview_seek_to_x(p.x);
                invalidate_preview_media();
                return;
            }
        }
        if (dragging_scroll_) {
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                dragging_scroll_ = false;
                ReleaseCapture();
            } else {
                const Grid g = grid();
                const float track = height(g.view) - 8 - height(scroll_thumb_);
                if (track > 0) {
                    scroll_ = target_scroll_ = std::clamp(drag_scroll_start_ + (p.y - drag_y_) / track * max_scroll(), 0.0f, max_scroll());
                }
                invalidate();
                return;
            }
        }
        if (selecting_text_) {
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                selecting_text_ = false;
                ReleaseCapture();
            } else {
                Canvas cv{target_, sh_.fonts, hits_};
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
            if (vid != hover_.id || vidx != hover_.index) {
                queue_hover_dirty(hover_.id, hover_.index);
                queue_hover_dirty(hover_.prev_id, hover_.prev_index);
                queue_hover_dirty(vid, vidx);
            }
            const bool vis_changed = hover_.set(vid, vidx, animate);
            std::wstring tt, ts;
            const bool can_tip = !menu_open_ && !drop_open_ && !drag_pending_ && !dragging_out_ && tooltip_for(id, idx, tt, ts);
            tooltip_.on_hover_changed(hwnd_, TIMER_TOOLTIP, h, can_tip, animate);
            const bool tip_fading = animate && tooltip_.alpha.active();
            if (show_help() != was_help) {
                full_dirty_ = true;
                apply_height();
            }
            if ((vis_changed && animate) || tip_fading) start_anim();
            else if (vis_changed) invalidate();
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
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
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
            if (id != IdPvBar && id != IdPvPlay && id != IdPvMute && id != IdPvSeek) invalidate();
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
                Canvas cv{target_, sh_.fonts, hits_};
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
            case IdSettings:
                if (sh_.open_settings) sh_.open_settings();
                else hide();
                return;
            case IdFullscreen: toggle_fullscreen(); return;
            case IdClose: hide(); return;
            case IdSimilar: find_similar(items_[idx]); return;
            case IdCardPreview: open_preview(idx); return;
            case IdCard:
            case IdCardName:
            case IdCardDir:
            case IdCardMoreTags:
            case IdTagChip: {
                const uint64_t cid = items_[idx].pt.id;
                if (dbl) {
                    open_file(item_path(items_[idx]));
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
                    target_scroll_ = std::clamp((p.y - g.view.top) / height(g.view) * grid().content_h - height(g.view) / 2, 0.0f, max_scroll());
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
                Canvas cv{target_, sh_.fonts, hits_};
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
        target_scroll_ = std::clamp(target_scroll_ - delta * 0.9f, 0.0f, max_scroll());
        start_anim();
    }

    void move_focus(int dx, int dy) {
        if (items_.empty()) return;
        const Grid g = grid();
        int f = focus_ < 0 ? 0 : focus_ + dx + dy * g.cols;
        f = std::clamp(f, 0, static_cast<int>(items_.size()) - 1);
        focus_ = f;
        if (GetKeyState(VK_SHIFT) < 0) selected_.insert(items_[f].pt.id);
        else selected_ = {items_[f].pt.id};
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
            if (menu_open_ || drop_open_ || help_pinned_) close_popups();
            else if (preview_ >= 0) close_preview();
            else if (fullscreen_) toggle_fullscreen();
            else if (!selected_.empty() && !field_.focused) {
                selected_.clear();
                focus_ = -1;
                field_.focused = true;
            } else hide();
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
        if (vk == VK_SPACE && !field_.focused && focus_ >= 0 && focus_ < static_cast<int>(items_.size())) {
            swallow_space_ = true;
            open_preview(focus_);
            return;
        }
        if (vk == VK_RETURN) {
            if (!field_.focused && focus_ >= 0) menu_action_selected(ActOpen);
            else run_search(0);
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
        if (field_.on_key(hwnd_, vk, handled)) schedule_search();
        else update_dropdown();
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
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        if (fullscreen_) return HTCLIENT;
        const int b = static_cast<int>(6 * target_.scale());
        const bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, btm = pt.y >= rc.bottom - b;
        const bool vert = expanded();
        if (t && l && vert) return HTTOPLEFT;
        if (t && r && vert) return HTTOPRIGHT;
        if (btm && l && vert) return HTBOTTOMLEFT;
        if (btm && r && vert) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t && vert) return HTTOP;
        if (btm && vert) return HTBOTTOM;
        const D2D1_POINT_2F p{pt.x / target_.scale(), pt.y / target_.scale()};
        if (p.y < HEADER_H + STATUS_H && !menu_open_ && !drop_open_ && !show_help()) {
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
            case WM_SIZE:
                if (wp != SIZE_MINIMIZED) {
                    target_.resize(LOWORD(lp), HIWORD(lp));
                    scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
                    full_dirty_ = true;
                    render();
                }
                return 0;
            case WM_ENTERSIZEMOVE: in_sizemove_ = true; return 0;
            case WM_EXITSIZEMOVE:
                in_sizemove_ = false;
                save_rect();
                if (sh_.gfx.adapter_stale(hwnd_)) recover();
                full_dirty_ = true;
                render();
                return 0;
            case WM_DISPLAYCHANGE:
                if (sh_.gfx.adapter_stale(hwnd_)) recover();
                invalidate();
                return 0;
            case WM_DPICHANGED: {
                full_dirty_ = true;
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                const float s = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
                mmi->ptMinTrackSize = {static_cast<LONG>(520 * s), static_cast<LONG>((HEADER_H + STATUS_H) * s)};
                return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE && sh_.settings.hide_on_focus_loss && !fullscreen_ && !sh_.test_mode && !dragging_out_) {
                    if (GetTickCount64() - shown_tick_ > 250) {
                        if (visible() && !fading_out_) {
                            GetCursorPos(&blur_hide_cursor_);
                            blur_hide_tick_ = GetTickCount64();
                        }
                        hide();
                    }
                }
                return 0;
            case WM_CAPTURECHANGED:
                selecting_text_ = false;
                dragging_scroll_ = false;
                dragging_seek_ = false;
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
                    const Hit* h = hit_test(hits_, {pt.x / target_.scale(), pt.y / target_.scale()});
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
                if (raw_hover_id_ != IdNone || hover_.id != IdNone) {
                    const bool was_help = show_help();
                    clear_hover(animate);
                    if (show_help() != was_help) {
                        full_dirty_ = true;
                        apply_height();
                    }
                    if (animate) start_anim();
                    else invalidate();
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
                if (dragging_seek_) dragging_seek_ = false;
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
                        field_.caret_on = !field_.caret_on;
                        menu_field_.caret_on = field_.caret_on;
                        if (field_.focused || menu_open_) invalidate();
                        return 0;
                    case TIMER_DEBOUNCE: run_search(0); return 0;
                    case TIMER_STATUS:
                        if (sh_.poll_indexer()) invalidate();
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
                        std::wstring tt, ts;
                        const bool allow = raw_hover_id_ != IdNone && !menu_open_ && !drop_open_ && !drag_pending_ && !dragging_out_ && tooltip_for(raw_hover_id_, raw_hover_index_, tt, ts);
                        const bool animate = visible() && !sh_.test_mode;
                        if (tooltip_.on_timer(hwnd_, TIMER_TOOLTIP, allow, std::move(tt), std::move(ts), animate)) {
                            if (animate) start_anim();
                            else invalidate();
                        }
                        return 0;
                    }
                    case TIMER_GIF: on_gif_timer(); return 0;
                    case TIMER_VIDEO: on_video_timer(); return 0;
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
                    std::unique_ptr<std::function<void()>> fn{reinterpret_cast<std::function<void()>*>(lp)};
                    (*fn)();
                }
                return 0;
            case WM_CLOSE: hide(); return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    Shared& sh_;
    HWND hwnd_ = nullptr;
    WindowTarget target_;
    std::vector<Hit> hits_;
    HCURSOR cursor_arrow_ = nullptr, cursor_hand_ = nullptr, cursor_ibeam_ = nullptr;
    bool rendering_ = false;
    bool dirty_ = false; // state changed since the last frame
    bool full_dirty_ = true;
    std::vector<D2D1_RECT_F> pending_dirty_;
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
    RECT restore_rect_{};
    bool in_sizemove_ = false;

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
    UINT thumb_px_ = 0;      // card thumbnail size last requested by the grid
    bool swallow_space_ = false;
    bool swallow_dbl_ = false;

    uint64_t gfx_epoch_ = 0;
    bool video_muted_ = false;
    bool dragging_seek_ = false;
    float seek_drag_ratio_ = 0.0f;
    D2D1_RECT_F pv_media_rect_{};
    D2D1_RECT_F pv_ctrl_rect_{};
    D2D1_RECT_F pv_seek_rect_{};
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
