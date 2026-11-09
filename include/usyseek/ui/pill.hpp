#pragma once

#include "shared.hpp"
#include "textfield.hpp"

#include <shellscalingapi.h>

#include <set>
#include <unordered_set>

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

        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, PILL_CLASS, APP_NAME,
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        style_window(hwnd_, DWMSBT_TRANSIENTWINDOW);
        const COLORREF border = RGB(0x44, 0x41, 0x48);
        DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
        if (!target_.attach(sh_.gfx, hwnd_)) return false;

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
        if (!visible()) place();
        shown_tick_ = GetTickCount64();
        selecting_text_ = false;
        dragging_scroll_ = false;
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        field_.focused = true;
        field_.select_all();
        caret_reset();
        sh_.refresh_tags();
        sh_.poll_indexer();
        SetTimer(hwnd_, TIMER_STATUS, 1000, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
        render();
    }

    void hide() {
        selecting_text_ = false;
        dragging_scroll_ = false;
        close_popups();
        KillTimer(hwnd_, TIMER_STATUS);
        ShowWindow(hwnd_, SW_HIDE);
        if (sh_.visibility_changed) sh_.visibility_changed();
    }

    void toggle() {
        if (visible() && GetForegroundWindow() == hwnd_) hide();
        else show();
    }

    void render() {
        if (rendering_) return;
        if (!ensure_target()) return;
        rendering_ = true;
        const bool anim = step_anim();
        auto* dc = target_.dc();
        hits_.clear();
        dc->BeginDraw();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        Canvas cv{dc, sh_.fonts, hits_};
        draw(cv);
        if (!target_.present(!in_sizemove_)) recover();
        rendering_ = false;
        if (anim || searching_ || loading_more_) InvalidateRect(hwnd_, nullptr, FALSE);
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
    static constexpr UINT_PTR TIMER_CARET = 1, TIMER_DEBOUNCE = 2, TIMER_STATUS = 4, TIMER_DEVICE = 5;

    enum Id {
        IdNone = 0,
        IdField,
        IdChip,
        IdTagBtn,
        IdSettings,
        IdFullscreen,
        IdClose,
        IdCard,
        IdSimilar,
        IdTagChip,
        IdScrollbar,
        IdMenu,
        IdMenuItem,
        IdMenuTag,
        IdMenuField,
        IdDrop,
        IdDropItem,
        IdGrid,
    };
    enum MenuAction { ActOpen, ActReveal, ActSimilar, ActCopy };

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

    [[nodiscard]] float dropdown_height_dip() const noexcept {
        if (!drop_open_) return 0.0f;
        const float row = 32.0f;
        const float h = drop_items_.empty() ? 44.0f : 8.0f + row * static_cast<float>(drop_items_.size());
        return (HEADER_H - 4.0f) + h + 12.0f;
    }

    [[nodiscard]] float current_height_dip() const {
        if (!expanded()) return std::max(HEADER_H + STATUS_H, dropdown_height_dip());
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
        render();
    }

    // ------------------------------------------------------------------------------------------
    // Device handling
    // ------------------------------------------------------------------------------------------

    bool ensure_target() {
        if (target_.ready()) return true;
        if (!sh_.gfx.ready() || sh_.gfx.device_lost()) {
            if (!sh_.gfx.recover(hwnd_)) {
                SetTimer(hwnd_, TIMER_DEVICE, 250, nullptr);
                return false;
            }
        }
        if (!target_.rebuild()) {
            SetTimer(hwnd_, TIMER_DEVICE, 250, nullptr);
            return false;
        }
        return true;
    }

    void recover() {
        // Present failed: device removed/reset or adapter gone. Rebuild everything on the next frame.
        sh_.gfx.recover(hwnd_);
        sh_.thumbs.reset_gpu();
        target_.release();
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
        apply_height();
        SetTimer(hwnd_, TIMER_DEBOUNCE, 140, nullptr);
    }

    void run_search(uint64_t offset) {
        KillTimer(hwnd_, TIMER_DEBOUNCE);
        if (offset == 0) {
            const std::wstring q = trim(field_.text);
            if (q.empty()) {
                ++seq_;
                items_.clear();
                selected_.clear();
                error_.clear();
                searching_ = false;
                scroll_ = target_scroll_ = 0.0f;
                apply_height();
                render();
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
        render();
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
            items_.clear();
            selected_.clear();
            focus_ = -1;
            scroll_ = target_scroll_ = 0.0f;
        }
        for (auto& p : r->points) items_.push_back(make_item(std::move(p)));
        apply_height();
        render();
    }

    void on_thumbs() {
        if (target_.ready() && sh_.thumbs.process(target_.dc())) render();
    }

    void on_indexer_changed() {
        sh_.poll_indexer();
        sh_.refresh_tags();
        render();
    }

    void on_tags_refreshed() {
        if (drop_open_) update_dropdown();
        if (visible()) render();
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

    void scroll_to(int i) {
        if (i < 0) return;
        const Grid g = grid();
        const D2D1_RECT_F r = card_rect(g, i);
        if (r.top < g.view.top + 4) target_scroll_ = scroll_ + (r.top - g.view.top - PAD);
        else if (r.bottom > g.view.bottom - 4) target_scroll_ = scroll_ + (r.bottom - g.view.bottom + PAD);
        target_scroll_ = std::clamp(target_scroll_, 0.0f, max_scroll());
        start_anim();
    }

    void start_anim() {
        last_anim_ = std::chrono::steady_clock::now();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool step_anim() {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_anim_).count(), 0.001f, 0.05f);
        last_anim_ = now;
        const float d = target_scroll_ - scroll_;
        if (std::abs(d) < 0.5f) {
            scroll_ = target_scroll_;
            return false;
        }
        scroll_ += d * (1.0f - std::exp(-dt * 22.0f));
        if (std::abs(target_scroll_ - scroll_) < 0.5f) {
            scroll_ = target_scroll_;
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Drawing
    // ------------------------------------------------------------------------------------------

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        cv.fill(D2D1::RectF(0, 0, sz.width, sz.height), with_alpha(color::bg, 0.80f));
        draw_header(cv, sz);
        draw_status(cv, sz);
        if (expanded()) draw_grid(cv);
        if (drop_open_) draw_dropdown(cv);
        if (menu_open_) draw_menu(cv);
    }

    void icon_button(Canvas& cv, D2D1_RECT_F r, wchar_t g, int id, bool active = false) {
        const bool hov = hover_id_ == id;
        if (active) cv.fill_round(r, height(r) / 2, with_alpha(color::primary, 0.16f));
        else if (hov) cv.fill_round(r, height(r) / 2, with_alpha(color::on_surface, 0.08f));
        cv.icon(g, r, active ? color::primary : hov ? color::on_surface : color::on_surface_variant);
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
        float cx = x + 3;
        for (int i = 0; i < 3; ++i) {
            const D2D1_RECT_F r = rect(cx, cy - 14, widths[i], 28);
            const bool on = chip_ == i - 1;
            if (on) cv.fill_round(r, 14, color::primary_container);
            else if (hover_id_ == IdChip && hover_index_ == i) cv.fill_round(r, 14, with_alpha(color::on_surface, 0.07f));
            cv.text(labels[i], r, Font::SmallSemi, on ? color::on_primary_container : color::on_surface_variant, Align::Center);
            cv.hit(r, IdChip, i);
            cx += widths[i];
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
        if (st == TextEncoder::State::Loading) return color::primary;
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
            piece(L"Indexing ", color::outline);
            if (d.phase == Phase::Embedding) piece(format_count(d.embedded) + L" / " + format_count(d.found), color::amber, Font::SmallSemi);
            else piece(phase_label(d.phase), color::amber, Font::SmallSemi);
        } else {
            piece(L"Last indexed ", color::outline);
            piece(format_ago(sh_.index_state.last_index_unix), color::on_surface_variant, Font::SmallSemi);
        }
        if (sh_.db_count) {
            sep();
            piece(format_count(*sh_.db_count) + L" files", color::outline);
        }
        const auto enc = sh_.engine.encoder().state();
        if (enc == TextEncoder::State::Loading) {
            sep();
            piece(L"Loading model…", color::primary);
        } else if (enc == TextEncoder::State::Failed) {
            sep();
            piece(sh_.engine.encoder().error(), color::error);
        }
        if (!error_.empty()) {
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
            const wchar_t* msg = searching_ ? L"" : (error_.empty() ? L"No matches" : L"");
            cv.text(msg, D2D1::RectF(g.view.left, g.view.top, g.view.right, g.view.top + 120), Font::Body, color::outline, Align::Center);
            if (searching_) draw_spinner(cv, {(g.view.left + g.view.right) / 2, g.view.top + 60});
        }

        const int first_row = std::max(0, static_cast<int>((scroll_ - PAD) / (g.card_h + GAP)));
        const int last_row = static_cast<int>((scroll_ + height(g.view)) / (g.card_h + GAP)) + 1;
        const UINT px = static_cast<UINT>(std::max(g.card_w, g.thumb_h * 1.6f) * target_.scale());
        // Prefetch one row beyond the viewport in both directions
        for (int i = std::max(0, (first_row - 1) * g.cols); i < std::min<int>(static_cast<int>(items_.size()), (last_row + 2) * g.cols); ++i) {
            const bool visible = i >= first_row * g.cols && i < (last_row + 1) * g.cols;
            if (visible) draw_card(cv, g, i);
        }
        // Request visible thumbnails last so they're served first (LIFO)
        for (int i = std::min<int>(static_cast<int>(items_.size()), (last_row + 2) * g.cols) - 1; i >= std::max(0, (first_row - 1) * g.cols); --i) {
            if (!sh_.thumbs.get(items_[i].pt.id)) sh_.thumbs.request(items_[i].pt.id, item_path(items_[i]), px);
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
            const bool active = hover_id_ == IdScrollbar || dragging_scroll_;
            const D2D1_RECT_F tr = D2D1::RectF(g.view.right - (active ? 9 : 7), ty, g.view.right - 3, ty + thumb_h);
            cv.fill_round(tr, 3, with_alpha(color::on_surface_variant, active ? 0.55f : 0.28f));
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

    void draw_card(Canvas& cv, const Grid& g, int i) {
        Item& it = items_[i];
        const D2D1_RECT_F r = card_rect(g, i);
        const bool hov = hover_id_ >= IdCard && hover_id_ <= IdTagChip && hover_index_ == i;
        const bool sel = selected_.contains(it.pt.id);
        const bool focus = focus_ == i;

        cv.fill_round(r, 14, with_alpha(hov ? color::surface_high : color::surface_low, 0.92f));
        if (sel) cv.stroke_round(r, 14, color::primary, 2.0f);
        else if (focus) cv.stroke_round(r, 14, with_alpha(color::primary, 0.6f), 1.0f);
        else cv.stroke_round(r, 14, with_alpha(color::outline_variant, hov ? 0.9f : 0.45f));
        cv.hit(r, IdCard, i);

        // Thumbnail (cover-cropped through a reusable bitmap brush so the corners stay rounded)
        const D2D1_RECT_F tr = rect(r.left + 6, r.top + 6, g.card_w - 12, g.thumb_h);
        if (ID2D1Bitmap1* bmp = sh_.thumbs.get(it.pt.id)) {
            cv.fill_bitmap_round(bmp, tr, 10.0f);
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
        if (hov) {
            const D2D1_RECT_F sb = rect(tr.right - 36, tr.top + 8, 28, 28);
            const bool on = hover_id_ == IdSimilar;
            cv.fill_circle({sb.left + 14, sb.top + 14}, 14, hex(on ? 0x2b2930 : 0x000000, on ? 0.95f : 0.55f));
            cv.icon(glyph::similar, sb, on ? color::primary : hex(0xffffff), Font::IconSmall);
            cv.hit(sb, IdSimilar, i);
        }

        float y = tr.bottom + 10.0f;
        const float lx = r.left + 12.0f, rw = g.card_w - 24.0f;
        cv.text(it.name, rect(lx, y, rw, 18.0f), Font::BodySemi, color::on_surface);
        y += 20.0f;
        cv.text(it.dir, rect(lx, y, rw, 16.0f), Font::MonoSmall, color::outline);
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
            cv.fill_round(rect(tx, y, w, 20.0f), 10.0f, with_alpha(color::on_surface, 0.08f));
            cv.text(more, rect(tx, y, w, 20.0f), Font::Caption, color::on_surface_variant, Align::Center);
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
            if (prefix.empty() || w.find(prefix) != std::wstring::npos) drop_items_.push_back(tc);
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
        const std::wstring value = tag.find(L' ') != std::wstring::npos ? L"\"" + tag + L"\"" : tag;
        if (auto tok = tag_token()) {
            size_t end = field_.caret;
            while (end < field_.text.size() && !iswspace(field_.text[end])) ++end;
            field_.text.replace(tok->second, end - tok->second, value + L" ");
            field_.caret = field_.anchor = tok->second + value.size() + 1;
        } else {
            std::wstring t = trim(field_.text);
            if (!t.empty()) t += L' ';
            t += L"tag:" + value + L" ";
            field_.set(t);
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
        render();
    }

    void close_popups() {
        menu_open_ = false;
        drop_open_ = false;
        drop_from_button_ = false;
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
        render();
    }

    [[nodiscard]] D2D1_RECT_F menu_rect() const {
        const D2D1_SIZE_F sz = target_.size();
        const float h = 4 * 36.0f + 16 + 30 + std::min<size_t>(menu_tags_.size(), 10) * 32.0f + 48;
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
        static constexpr Act acts[] = {
            {glyph::open, L"Open", L"Enter"},
            {glyph::folder, L"Show in Explorer", L"Ctrl+E"},
            {glyph::similar, L"Find similar", L"Ctrl+S"},
            {glyph::copy, L"Copy path", L"Ctrl+C"},
        };
        float y = r.top + 6;
        for (int i = 0; i < 4; ++i) {
            const D2D1_RECT_F rr = rect(r.left + 6, y, MENU_W - 12, 36);
            if (hover_id_ == IdMenuItem && hover_index_ == i) cv.fill_round(rr, 8, with_alpha(color::on_surface, 0.08f));
            cv.icon(acts[i].g, rect(rr.left + 8, rr.top, 20, 36), color::on_surface_variant);
            cv.text(acts[i].label, D2D1::RectF(rr.left + 38, rr.top, rr.right - 60, rr.bottom), Font::Body, color::on_surface);
            cv.text(acts[i].key, D2D1::RectF(rr.right - 70, rr.top, rr.right - 10, rr.bottom), Font::Caption, color::outline, Align::Right);
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
            if (hover_id_ == IdMenuTag && hover_index_ == static_cast<int>(i)) cv.fill_round(rr, 8, with_alpha(color::on_surface, 0.08f));
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
        if (sel.empty() && menu_index_ >= 0 && menu_index_ < static_cast<int>(items_.size())) sel.push_back(&items_[menu_index_]);
        if (sel.empty()) return;
        close_popups();
        switch (a) {
            case ActOpen:
                for (size_t i = 0; i < sel.size() && i < 8; ++i) open_file(item_path(*sel[i]));
                break;
            case ActReveal: reveal_in_explorer(item_path(*sel.front())); break;
            case ActSimilar: find_similar(*sel.front()); break;
            case ActCopy: {
                std::wstring all;
                for (const auto* it : sel) {
                    if (!all.empty()) all += L"\r\n";
                    all += item_path(*it);
                }
                set_clipboard_text(hwnd_, all);
                break;
            }
        }
        render();
    }

    void find_similar(const Item& it) {
        field_.set(L"similar:" + item_path(it));
        field_.focused = true;
        run_search(0);
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
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
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
                render();
                return;
            }
        }
        if (selecting_text_) {
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                selecting_text_ = false;
                ReleaseCapture();
            } else {
                Canvas cv{target_.dc(), sh_.fonts, hits_};
                field_.click(cv, p.x, true);
                render();
                return;
            }
        }
        const Hit* h = hit_test(hits_, p);
        const int id = h ? h->id : IdNone;
        const int idx = h ? h->index : 0;
        if (id != hover_id_ || idx != hover_index_) {
            hover_id_ = id;
            hover_index_ = idx;
            render();
        }
    }

    void on_lbutton_down(D2D1_POINT_2F p, bool dbl) {
        selecting_text_ = false;
        dragging_scroll_ = false;
        SetFocus(hwnd_);
        const Hit* h = hit_test(hits_, p);
        const int id = h ? h->id : IdNone;
        const int idx = h ? h->index : 0;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;

        if (menu_open_ && id != IdMenu && id != IdMenuItem && id != IdMenuTag && id != IdMenuField) {
            close_popups();
            render();
            return;
        }
        if (drop_open_ && id != IdDrop && id != IdDropItem && id != IdTagBtn && id != IdField) {
            drop_open_ = false;
            drop_from_button_ = false;
            apply_height();
        }
        switch (id) {
            case IdField: {
                field_.focused = true;
                Canvas cv{target_.dc(), sh_.fonts, hits_};
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
                if (!trim(field_.text).empty()) run_search(0);
                break;
            case IdTagBtn:
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
            case IdCard:
            case IdTagChip: {
                const uint64_t cid = items_[idx].pt.id;
                if (dbl) {
                    open_file(item_path(items_[idx]));
                    return;
                }
                if (ctrl) {
                    if (!selected_.erase(cid)) selected_.insert(cid);
                } else if (shift && focus_ >= 0) {
                    selected_.clear();
                    for (int k = std::min(focus_, idx); k <= std::max(focus_, idx); ++k) selected_.insert(items_[k].pt.id);
                } else {
                    selected_ = {cid};
                }
                if (!shift) focus_ = idx;
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
                Canvas cv{target_.dc(), sh_.fonts, hits_};
                menu_field_.click(cv, p.x, shift);
                break;
            }
            case IdDropItem: accept_dropdown(idx); return;
            case IdGrid:
                selected_.clear();
                break;
            default: break;
        }
        render();
    }

    void on_rbutton_down(D2D1_POINT_2F p) {
        const Hit* h = hit_test(hits_, p);
        if (!h || (h->id != IdCard && h->id != IdTagChip && h->id != IdSimilar)) {
            if (menu_open_) {
                close_popups();
                render();
            }
            return;
        }
        if (!selected_.contains(items_[h->index].pt.id)) selected_ = {items_[h->index].pt.id};
        focus_ = h->index;
        open_menu(h->index, p);
    }

    void on_wheel(int delta) {
        if (!expanded() || menu_open_) return;
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
        render();
    }

    void on_key(UINT vk) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (vk == VK_ESCAPE) {
            if (menu_open_ || drop_open_) close_popups();
            else if (fullscreen_) toggle_fullscreen();
            else if (!selected_.empty() && !field_.focused) {
                selected_.clear();
                field_.focused = true;
            } else hide();
            render();
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
            render();
            return;
        }
        if (drop_open_ && !drop_items_.empty()) {
            if (vk == VK_DOWN || vk == VK_UP) {
                drop_sel_ = (drop_sel_ + (vk == VK_DOWN ? 1 : -1) + static_cast<int>(drop_items_.size())) % static_cast<int>(drop_items_.size());
                render();
                return;
            }
            if (vk == VK_RETURN || vk == VK_TAB) {
                accept_dropdown(drop_sel_);
                return;
            }
        }
        if (ctrl && !selected_.empty()) {
            if (vk == 'E') return menu_action_selected(ActReveal);
            if (vk == 'S') return menu_action_selected(ActSimilar);
            if (vk == 'C' && !field_.focused) return menu_action_selected(ActCopy);
            if (vk == 'A' && !field_.focused) {
                for (const auto& it : items_) selected_.insert(it.pt.id);
                render();
                return;
            }
        }
        if (ctrl && vk == 'F') {
            field_.focused = true;
            field_.select_all();
            render();
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
                render();
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
        render();
    }

    void menu_action_selected(int a) {
        menu_index_ = focus_;
        menu_action(a);
    }

    void on_char(wchar_t c) {
        if (c == L'\r' || c == 27 || c == L'\t') return;
        if (menu_open_) {
            menu_field_.on_char(c);
            caret_reset();
            render();
            return;
        }
        if (!field_.focused) {
            field_.focused = true;
            focus_ = -1;
        }
        if (field_.on_char(c)) schedule_search();
        caret_reset();
        render();
    }

    LRESULT hit_test_nc(LPARAM lp) {
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
        if (p.y < HEADER_H + STATUS_H && !menu_open_ && !drop_open_) {
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
                render();
                EndPaint(hwnd_, &ps);
                if (std::abs(target_scroll_ - scroll_) >= 0.5f || searching_ || loading_more_) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            case WM_SIZE:
                if (wp != SIZE_MINIMIZED) {
                    target_.resize(LOWORD(lp), HIWORD(lp));
                    scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
                    render();
                }
                return 0;
            case WM_ENTERSIZEMOVE: in_sizemove_ = true; return 0;
            case WM_EXITSIZEMOVE:
                in_sizemove_ = false;
                save_rect();
                if (sh_.gfx.adapter_stale(hwnd_)) recover();
                render();
                return 0;
            case WM_DISPLAYCHANGE:
                if (sh_.gfx.adapter_stale(hwnd_)) recover();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_DPICHANGED: {
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
                if (LOWORD(wp) == WA_INACTIVE && sh_.settings.hide_on_focus_loss && !fullscreen_ && !sh_.test_mode) {
                    if (GetTickCount64() - shown_tick_ > 250) hide();
                }
                return 0;
            case WM_CAPTURECHANGED:
                selecting_text_ = false;
                dragging_scroll_ = false;
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd_, &pt);
                    const Hit* h = hit_test(hits_, {pt.x / target_.scale(), pt.y / target_.scale()});
                    const int id = h ? h->id : 0;
                    SetCursor(id == IdField || id == IdMenuField ? cursor_ibeam_ : (id == IdNone || id == IdGrid || id == IdMenu || id == IdDrop || id == IdCard) ? cursor_arrow_ : cursor_hand_);
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: on_mouse_move(to_dip(lp)); return 0;
            case WM_MOUSELEAVE:
                tracking_ = false;
                if (hover_id_) {
                    hover_id_ = IdNone;
                    render();
                }
                return 0;
            case WM_LBUTTONDOWN: on_lbutton_down(to_dip(lp), false); return 0;
            case WM_LBUTTONDBLCLK: on_lbutton_down(to_dip(lp), true); return 0;
            case WM_LBUTTONUP:
                selecting_text_ = false;
                if (dragging_scroll_) dragging_scroll_ = false;
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
                        if (field_.focused || menu_open_) render();
                        return 0;
                    case TIMER_DEBOUNCE: run_search(0); return 0;
                    case TIMER_STATUS:
                        sh_.poll_indexer();
                        render();
                        return 0;
                    case TIMER_DEVICE:
                        KillTimer(hwnd_, TIMER_DEVICE);
                        render();
                        return 0;
                }
                return 0;
            case WM_APP_THUMBS: on_thumbs(); return 0;
            case WM_APP_CALL:
                if (wp == 1) {
                    run_search(items_.size());
                } else if (wp == 2) {
                    render();
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
    ULONGLONG shown_tick_ = 0;

    TextField field_;
    int chip_ = -1;
    std::wstring last_query_;
    uint64_t seq_ = 0;
    bool searching_ = false;
    bool loading_more_ = false;
    bool has_more_ = false;
    bool semantic_ = false;
    double last_ms_ = 0;
    std::wstring error_;
    std::vector<Item> items_;

    std::unordered_set<uint64_t> selected_;
    int focus_ = -1;
    int hover_id_ = IdNone;
    int hover_index_ = 0;
    bool tracking_ = false;
    bool selecting_text_ = false;

    float scroll_ = 0, target_scroll_ = 0;
    std::chrono::steady_clock::time_point last_anim_ = std::chrono::steady_clock::now();
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
};

} // namespace usyseek::ui
