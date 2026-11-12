#pragma once

#include "shared.hpp"
#include "flow.hpp"
#include "textfield.hpp"
#include "../http.hpp"
#include "../path_rules.hpp"
#include "../torch_index.hpp"

#include <shobjidl.h>

namespace usyseek {

struct NugetVersionIndex {
    std::vector<std::string> versions;
};

} // namespace usyseek

namespace usyseek::ui {

class Flow {
public:
    Flow(float gap_x, float gap_y) : gap_x_{gap_x}, gap_y_{gap_y} {}

    void add(float w, float h, std::function<void(D2D1_RECT_F)> draw, bool grow = false) {
        items_.push_back({w, h, grow});
        draws_.push_back(std::move(draw));
    }

    void add_group(const Flow& inner, float avail, bool grow = false) {
        const float group_w = std::min(inner.natural_width(), avail);
        add(group_w, inner.height(group_w), [inner](D2D1_RECT_F r) { inner.draw(r.left, r.top, width(r)); }, grow);
    }

    [[nodiscard]] float natural_width() const {
        float total = 0.0f;
        for (const FlowItem& item : items_) {
            total += item.width;
        }
        return items_.empty() ? 0.0f : total + gap_x_ * static_cast<float>(items_.size() - 1);
    }

    [[nodiscard]] float height(float avail) const {
        return flow_layout(items_, gap_x_, gap_y_, avail).height;
    }

    void draw(float x, float y, float avail) const {
        const FlowResult layout = flow_layout(items_, gap_x_, gap_y_, avail);
        for (size_t i = 0; i < items_.size(); ++i) {
            const FlowPlacement& p = layout.placements[i];
            draws_[i](rect(x + p.x, y + p.y, p.width, items_[i].height));
        }
    }

private:
    float gap_x_;
    float gap_y_;
    std::vector<FlowItem> items_;
    std::vector<std::function<void(D2D1_RECT_F)>> draws_;
};

class SettingsWindow {
public:
    explicit SettingsWindow(Shared& s) : sh_{s} {}

    bool create(HINSTANCE inst) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &SettingsWindow::wndproc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
        wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(101));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = SETTINGS_CLASS;
        RegisterClassExW(&wc);

        const UINT dpi = GetDpiForSystem();
        const float s = (dpi > 0 ? static_cast<float>(dpi) : 96.0f) / 96.0f;
        RECT rc{0, 0, static_cast<LONG>(920 * s), static_cast<LONG>(720 * s)};
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);

        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, SETTINGS_CLASS, L"usyseek — Settings & Indexing",
                                style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        style_window(hwnd_, DWMSBT_MAINWINDOW, true);
        if (!target_.attach(sh_.gfx, hwnd_)) return false;

        cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
        cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);

        exclude_field_.placeholder = L"Pattern or path (e.g. *\\cache or D:\\Temp)";
        model_field_.placeholder = L"PE-Core-L-14-336";
        batch_field_.placeholder = L"Custom";
        py_ver_field_.placeholder = L"3.14";
        arch_field_.placeholder = L"gfx1030";
        cuda_field_.placeholder = L"cu126";
        torch_field_.placeholder = L"Optional custom pip args (e.g. --index-url ... torch==2.7.1 torchvision==0.22.1)";
        sync_fields();
        tab_anim_.snap(static_cast<float>(tab_));
        sync_switch_targets(false);
        probe_venv();
        scan_models();
        return true;
    }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool visible() const noexcept { return IsWindowVisible(hwnd_) != FALSE; }
    WindowTarget& target() noexcept { return target_; }

    void show(int tab = -1) {
        if (tab >= 0) {
            tab_ = tab;
            scroll_ = target_scroll_ = 0.0f;
        }
        sync_fields();
        tab_anim_.snap(static_cast<float>(tab_));
        sync_switch_targets(false);
        probe_venv();
        scan_models();
        sh_.poll_indexer(true);
        sh_.refresh_tags();
        sh_.refresh_hotkey_state();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        SetTimer(hwnd_, TIMER_POLL, 500, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
        render();
    }

    void hide() {
        stop_recording();
        if (active_field_) {
            commit_fields();
            active_field_->focused = false;
            active_field_ = nullptr;
        }
        snap_anims();
        KillTimer(hwnd_, TIMER_CARET);
        KillTimer(hwnd_, TIMER_POLL);
        KillTimer(hwnd_, TIMER_DEVICE);
        ShowWindow(hwnd_, SW_HIDE);
        ValidateRect(hwnd_, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
    }

    void set_tab(int t) {
        const bool animate = visible() && !sh_.test_mode;
        tab_ = t;
        tab_anim_.set(static_cast<float>(t), animate);
        scroll_ = target_scroll_ = 0.0f;
        active_field_ = nullptr;
        if (animate) start_anim();
        invalidate();
    }

    /// Asks for a repaint; calls made before the next WM_PAINT collapse into one frame, and input is handled first.
    void invalidate() {
        dirty_ = true;
        if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void render() {
        if (rendering_ || (!visible() && !sh_.test_mode)) return;
        if (!ensure_target()) return;
        rendering_ = true;
        dirty_ = false;
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
        if (visible() && anim) InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:
    static constexpr float TABS_H = 52.0f;
    static constexpr float PAD = 24.0f;
    static constexpr float CARD_GAP = 16.0f;
    static constexpr UINT_PTR TIMER_CARET = 1, TIMER_POLL = 3, TIMER_DEVICE = 4;

    enum Id : uint8_t {
        IdNone = 0,
        IdTab0,
        IdTab1,
        IdToggleIndex,
        IdOpenQdrantUi,
        IdClearDatabase,
        IdClearFailed,
        IdOpenFailedLog,
        IdFailedRow,
        IdScopeAll,
        IdScopeCustom,
        IdAddInclude,
        IdRemoveInclude,
        IdToggleImages,
        IdToggleVideos,
        IdMinKbStep,
        IdVideoFramesStep,
        IdAddExcludeFolder,
        IdAddExcludePattern,
        IdExcludeField,
        IdRemoveExclude,
        // Tab 2
        IdToggleHotkey,
        IdRecordHotkey,
        IdResetHotkey,
        IdToggleStartup,
        IdToggleHideFocus,
        IdIdleStep,
        IdThumbFit,
        IdBrowseModelDir,
        IdModelChip,
        IdModelField,
        IdRepoField,
        IdDownloadModel,
        IdDeviceAuto,
        IdDeviceCpu,
        IdBatchPreset,
        IdBatchField,
        IdResetAutoBatch,
        IdBrowseScript,
        IdClearScript,
        IdBrowseVenv,
        IdBrowseBasePy,
        IdPyVerChip,
        IdPyVerField,
        IdDownloadNugetPy,
        IdBackendChip,
        IdArchField,
        IdCudaPreset,
        IdCudaField,
        IdTorchField,
        IdOpenTerminal,
        IdUpdateVenv,
        IdRecreateVenv,
        IdDeleteVenv,
        IdScrollbar,
    };

    void sync_fields() {
        sh_.settings.launch_on_startup = is_startup_enabled();
        model_field_.set(to_wide(sh_.settings.model_name));
        batch_field_.set(sh_.settings.batch_size > 0 ? std::to_wstring(sh_.settings.batch_size) : L"");
        py_ver_field_.set(to_wide(sh_.settings.python_version.empty() ? "3.14" : sh_.settings.python_version));
        arch_field_.set(to_wide(sh_.settings.rocm_arch));
        cuda_field_.set(to_wide(sh_.settings.cuda_version));
        torch_field_.set(to_wide(sh_.settings.torch_pip_args));
    }

    void scan_models() {
        models_.clear();
        std::error_code ec;
        const fs::path dir = to_wide(sh_.settings.model_dir);
        if (fs::exists(dir, ec)) {
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == L".safetensors") {
                    models_.push_back(to_utf8(e.path().stem().wstring()));
                }
            }
        }
        if (std::find(models_.begin(), models_.end(), sh_.settings.model_name) == models_.end()) {
            models_.insert(models_.begin(), sh_.settings.model_name);
        }
        refresh_model_files();
    }

    /// Which files of the selected model are on disk. Returns true when that changed.
    bool refresh_model_files() {
        const fs::path dir = to_wide(sh_.settings.model_dir);
        const std::wstring name = to_wide(sh_.settings.model_name);
        std::error_code ec;
        ModelFiles now;
        now.weights = fs::exists(dir / (name + L".safetensors"), ec);
        now.weights_bytes = now.weights ? fs::file_size(dir / (name + L".safetensors"), ec) : 0;
        if (ec) now.weights_bytes = 0;
        now.tokenizer = fs::exists(dir / (name + L"-tokenizer.json"), ec) || fs::exists(dir / L"tokenizer.json", ec);
        now.onnx = fs::exists(dir / (name + L"-text.onnx"), ec);
        const bool changed = now != model_files_;
        model_files_ = now;
        return changed;
    }

    void probe_venv() {
        const fs::path vdir = to_wide(sh_.settings.venv_dir);
        venv_ = read_venv(vdir);
        venv_pkgs_.clear();
        if (!venv_.exists) return;
        std::error_code ec;
        const fs::path sp = venv_.site_packages.empty() ? (vdir / L"Lib" / L"site-packages") : fs::path{venv_.site_packages};
        if (!fs::exists(sp, ec)) return;
        std::vector<std::wstring> found;
        for (const auto& e : fs::directory_iterator(sp, ec)) {
            if (!e.is_directory(ec)) continue;
            const std::wstring name = e.path().filename().wstring();
            if (!name.ends_with(L".dist-info")) continue;
            const std::wstring stem = name.substr(0, name.size() - 10);
            for (std::wstring_view want : {L"torch-", L"open_clip_torch-", L"timm-", L"safetensors-", L"onnxruntime-"}) {
                if (_wcsnicmp(stem.c_str(), want.data(), want.size()) == 0) {
                    std::wstring label = stem;
                    std::replace(label.begin(), label.end(), L'_', L'-');
                    found.push_back(std::move(label));
                }
            }
        }
        std::sort(found.begin(), found.end());
        for (size_t i = 0; i < found.size(); ++i) {
            if (i) venv_pkgs_ += L", ";
            venv_pkgs_ += found[i];
        }
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
        sh_.gfx.recover(hwnd_);
        sh_.thumbs.reset_gpu();
        target_.release();
        SetTimer(hwnd_, TIMER_DEVICE, 100, nullptr);
    }

    // ------------------------------------------------------------------------------------------
    // Drawing primitives
    // ------------------------------------------------------------------------------------------

    bool hovered(int id, int idx = 0) const noexcept {
        return hover_id_ == id && hover_index_ == idx;
    }

    void draw_button(Canvas& cv, D2D1_RECT_F r, wchar_t icon_g, std::wstring_view label, int id, int idx = 0,
                     int style = 0 /* 0=secondary, 1=primary, 2=danger, 3=ghost */) {
        const bool hov = hovered(id, idx);
        D2D1_COLOR_F bg = color::surface_high;
        D2D1_COLOR_F fg = color::on_surface;
        D2D1_COLOR_F border = color::outline_variant;
        if (style == 1) {
            bg = hov ? hex(0xb8d4ff) : color::primary;
            fg = color::on_primary;
            border = bg;
        } else if (style == 2) {
            bg = hov ? with_alpha(color::error, 0.22f) : with_alpha(color::error_container, 0.45f);
            fg = color::error;
            border = with_alpha(color::error, 0.45f);
        } else if (style == 3) {
            bg = hov ? with_alpha(color::on_surface, 0.08f) : hex(0, 0.0f);
            fg = color::on_surface_variant;
            border = hex(0, 0.0f);
        } else if (hov) {
            bg = color::surface_highest;
        }
        const float rad = std::min(10.0f, height(r) * 0.5f);
        if (bg.a > 0.0f) cv.fill_round(r, rad, bg);
        if (border.a > 0.0f && style != 1) cv.stroke_round(r, rad, border);
        if (icon_g) {
            const float tw = cv.measure(label, Font::SmallSemi);
            const float total = 18.0f + 6.0f + tw;
            const float sx = r.left + (width(r) - total) * 0.5f;
            cv.icon(icon_g, rect(sx, r.top, 18.0f, height(r)), fg, Font::IconSmall);
            cv.text(label, rect(sx + 24.0f, r.top, tw + 4.0f, height(r)), Font::SmallSemi, fg);
        } else {
            cv.text(label, r, Font::SmallSemi, fg, Align::Center);
        }
        cv.hit(r, id, idx);
    }

    void draw_switch(Canvas& cv, float right_x, float cy, bool on, int id, int idx = 0) {
        const D2D1_RECT_F r = rect(right_x - 44.0f, cy - 12.0f, 44.0f, 24.0f);
        const bool hov = hovered(id, idx);
        const float t = switch_anim_value(id, on);
        const D2D1_COLOR_F off_track = hov ? color::surface_highest : color::surface_high;
        cv.fill_round(r, 12.0f, lerp_color(off_track, color::primary, t));
        if (t < 1.0f) cv.stroke_round(r, 12.0f, with_alpha(color::outline, 1.0f - t));
        const float kx = lerp(r.left + 12.0f, r.right - 12.0f, t);
        const float kr = lerp(7.0f, 8.0f, t);
        cv.fill_circle({kx, cy}, kr, lerp_color(color::outline, color::on_primary, t));
        cv.hit(r, id, idx);
    }

    float text_height(Canvas& cv, std::wstring_view s, Font f, float w, float min_h) {
        return std::max(min_h, cv.measure_height(s, f, w));
    }

    float error_row_height(Canvas& cv, std::wstring_view message, float row_w) {
        return text_height(cv, message, Font::Caption, row_w - 22.0f, 20.0f);
    }

    void draw_error_row(Canvas& cv, std::wstring_view message, float x, float y, float row_w, float h) {
        cv.icon(glyph::warning, rect(x, y, 18.0f, 20.0f), color::error, Font::IconSmall);
        cv.text_wrap(message, rect(x + 22.0f, y, row_w - 22.0f, h), Font::Caption, color::error);
    }

    float mono_row_height(Canvas& cv, std::wstring_view s, float text_w, float min_h) {
        return std::max(min_h, 12.0f + cv.measure_height(s, Font::MonoSmall, text_w));
    }

    float button_width(Canvas& cv, std::wstring_view label, bool has_icon, float min_w) {
        return std::max(min_w, cv.measure(label, Font::SmallSemi) + (has_icon ? 24.0f : 0.0f) + 28.0f);
    }

    void text_middle(Canvas& cv, std::wstring_view s, D2D1_RECT_F r, Font f, D2D1_COLOR_F c) {
        const float th = cv.measure_height(s, f, width(r));
        cv.text_wrap(s, rect(r.left, r.top + std::max(0.0f, (height(r) - th) * 0.5f), width(r), th), f, c);
    }

    void flow_text(Canvas& cv, Flow& flow, std::wstring_view s, Font f, D2D1_COLOR_F c, float avail, float line_h,
                   bool grow = false, Align a = Align::Left) {
        const float natural = cv.measure(s, f) + 4.0f;
        if (natural <= avail) {
            flow.add(natural, line_h, [&cv, text = std::wstring{s}, f, c, a](D2D1_RECT_F r) { cv.text(text, r, f, c, a); }, grow);
            return;
        }
        const float wrapped_h = text_height(cv, s, f, avail, line_h);
        flow.add(avail, wrapped_h, [&cv, text = std::wstring{s}, f, c, a](D2D1_RECT_F r) { cv.text_wrap(text, r, f, c, a); });
    }

    struct CardHeader {
        std::wstring title;
        std::wstring caption;
        D2D1_COLOR_F title_color;
        float side_w;
        float side_h;
        bool beside;
        float text_w;
        float title_h;
        float caption_h;
        float h;
    };

    CardHeader make_header(Canvas& cv, float rw, std::wstring title, D2D1_COLOR_F title_color, std::wstring caption,
                           float side_w = 0.0f, float side_h = 0.0f) {
        CardHeader hd{std::move(title), std::move(caption), title_color, side_w, side_h};
        const float beside_w = rw - side_w - (side_w > 0.0f ? 12.0f : 0.0f);
        hd.beside = side_w <= 0.0f || beside_w >= 200.0f;
        hd.text_w = hd.beside ? beside_w : rw;
        hd.title_h = text_height(cv, hd.title, Font::Large, hd.text_w, 22.0f);
        hd.caption_h = hd.caption.empty() ? 0.0f : text_height(cv, hd.caption, Font::Small, hd.text_w, 18.0f);
        hd.h = hd.title_h + hd.caption_h + (hd.beside ? 0.0f : 8.0f + side_h);
        return hd;
    }

    D2D1_RECT_F draw_header(Canvas& cv, float lx, float y, float rw, const CardHeader& hd) {
        cv.text_wrap(hd.title, rect(lx, y + 16.0f, hd.text_w, hd.title_h), Font::Large, hd.title_color);
        cv.text_wrap(hd.caption, rect(lx, y + 16.0f + hd.title_h, hd.text_w, hd.caption_h), Font::Small, color::on_surface_variant);
        if (hd.beside) return rect(lx + rw - hd.side_w, y + 18.0f, hd.side_w, hd.side_h);
        return rect(lx, y + 16.0f + hd.title_h + hd.caption_h + 8.0f, hd.side_w, hd.side_h);
    }

    float radio_height(Canvas& cv, float w, std::wstring_view title, std::wstring_view desc) {
        return 8.0f + text_height(cv, title, Font::BodySemi, w - 54.0f, 20.0f) + text_height(cv, desc, Font::Caption, w - 54.0f, 18.0f) + 10.0f;
    }

    void draw_radio(Canvas& cv, D2D1_RECT_F r, bool on, std::wstring_view title, std::wstring_view desc, int id) {
        const bool hov = hovered(id);
        cv.fill_round(r, 12.0f, on ? with_alpha(color::primary_container, 0.32f) : (hov ? color::surface_high : color::surface_low));
        cv.stroke_round(r, 12.0f, on ? color::primary : color::outline_variant, on ? 1.5f : 1.0f);
        const D2D1_POINT_2F c{r.left + 22.0f, (r.top + r.bottom) * 0.5f};
        cv.stroke_circle(c, 8.0f, on ? color::primary : color::outline, 1.8f);
        if (on) cv.fill_circle(c, 4.5f, color::primary);
        const float title_h = text_height(cv, title, Font::BodySemi, width(r) - 54.0f, 20.0f);
        const float desc_h = text_height(cv, desc, Font::Caption, width(r) - 54.0f, 18.0f);
        cv.text_wrap(title, rect(r.left + 42.0f, r.top + 8.0f, width(r) - 54.0f, title_h), Font::BodySemi, color::on_surface);
        cv.text_wrap(desc, rect(r.left + 42.0f, r.top + 8.0f + title_h, width(r) - 54.0f, desc_h), Font::Caption, color::on_surface_variant);
        cv.hit(r, id);
    }

    float card_begin(Canvas& cv, float x, float y, float w, float h) {
        const D2D1_RECT_F r = rect(x, y, w, h);
        cv.fill_round(r, 16.0f, with_alpha(color::surface, 0.90f));
        cv.stroke_round(r, 16.0f, with_alpha(color::outline_variant, 0.65f));
        return y + h + CARD_GAP;
    }

    // ------------------------------------------------------------------------------------------
    // Main draw
    // ------------------------------------------------------------------------------------------

    struct TabDef {
        wchar_t g;
        const wchar_t* label;
        int id;
    };

    static std::array<TabDef, 2> tab_defs() {
        return {{
            {glyph::clock, L"Embeddings & Progress", IdTab0},
            {glyph::settings, L"Application & Venv Settings", IdTab1},
        }};
    }

    FlowResult tab_layout(float total_w) const {
        std::vector<FlowItem> items;
        for (const TabDef& t : tab_defs()) {
            items.push_back({sh_.fonts.measure(t.label, Font::BodySemi) + 46.0f, TABS_H - 16.0f});
        }
        return flow_layout(items, 8.0f, 8.0f, total_w - PAD * 2.0f);
    }

    [[nodiscard]] float tabs_height(float total_w) const {
        return tab_layout(total_w).height + 16.0f;
    }

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        cv.fill(D2D1::RectF(0, 0, sz.width, sz.height), with_alpha(color::bg, 0.88f));

        // Top tab bar
        draw_tabs(cv, sz);

        const D2D1_RECT_F view = D2D1::RectF(0, tabs_height(sz.width) + 1.0f, sz.width, sz.height);
        cv.push_clip(view);

        const float cw = std::min(860.0f, sz.width - PAD * 2.0f);
        const float cx = std::round((sz.width - cw) * 0.5f);
        float y = view.top + PAD - scroll_;

        if (tab_ == 0) {
            y = draw_tab_progress(cv, cx, y, cw);
        } else {
            y = draw_tab_settings(cv, cx, y, cw);
        }

        content_h_ = (y + scroll_ - view.top) + PAD;
        cv.pop_clip();

        // Scrollbar
        const float ms = max_scroll();
        if (ms > 0.0f) {
            const float track_h = height(view) - 8.0f;
            const float thumb_h = std::max(28.0f, track_h * height(view) / content_h_);
            const float ty = view.top + 4.0f + (track_h - thumb_h) * (scroll_ / ms);
            const bool active = hovered(IdScrollbar) || dragging_scroll_;
            const D2D1_RECT_F tr = D2D1::RectF(view.right - (active ? 9.0f : 7.0f), ty, view.right - 3.0f, ty + thumb_h);
            cv.fill_round(tr, 3.0f, with_alpha(color::on_surface_variant, active ? 0.55f : 0.28f));
            cv.hit(D2D1::RectF(view.right - 12.0f, view.top, view.right, view.bottom), IdScrollbar);
            scroll_thumb_ = tr;
        }
    }

    void draw_tabs(Canvas& cv, D2D1_SIZE_F sz) {
        const float strip_h = tabs_height(sz.width);
        cv.fill(D2D1::RectF(0, 0, sz.width, strip_h), with_alpha(color::surface_lowest, 0.75f));
        const auto tabs = tab_defs();
        const FlowResult layout = tab_layout(sz.width);
        D2D1_RECT_F rects[2]{};
        for (int i = 0; i < 2; ++i) {
            const FlowPlacement& p = layout.placements[i];
            rects[i] = rect(PAD + p.x, 8.0f + p.y, p.width, TABS_H - 16.0f);
        }
        const float t = std::clamp(tab_anim_.value, 0.0f, 1.0f);
        for (int i = 0; i < 2; ++i) {
            const float act = i == 0 ? 1.0f - t : t;
            if (act < 0.5f && hovered(tabs[i].id)) {
                cv.fill_round(rects[i], 10.0f, with_alpha(color::on_surface, 0.07f));
            }
        }
        cv.fill_round(lerp_rect(rects[0], rects[1], t), 10.0f, color::primary_container);
        for (int i = 0; i < 2; ++i) {
            const D2D1_RECT_F r = rects[i];
            const float act = i == 0 ? 1.0f - t : t;
            const float tw = cv.measure(tabs[i].label, Font::BodySemi);
            const D2D1_COLOR_F fg = lerp_color(color::on_surface_variant, color::on_primary_container, act);
            cv.icon(tabs[i].g, rect(r.left + 12.0f, r.top, 18.0f, height(r)), fg, Font::IconSmall);
            cv.text(tabs[i].label, rect(r.left + 34.0f, r.top, tw + 4.0f, height(r)), Font::BodySemi, fg);
            cv.hit(r, tabs[i].id);
        }
        cv.hline(0, sz.width, strip_h, with_alpha(color::outline_variant, 0.65f));
    }

    // ------------------------------------------------------------------------------------------
    // Tab 1: Embeddings & Progress
    // ------------------------------------------------------------------------------------------

    float draw_tab_progress(Canvas& cv, float x, float y, float w) {
        // 1. Hero status card
        {
            const float lx = x + 22.0f, rw = w - 44.0f;

            const bool running = sh_.indexer_running;
            const bool stopping = running && sh_.indexer_stopping;
            const auto& d = sh_.indexer;
            const DisplayedProgress shown = displayed_progress(running, d.embedded, d.found, sh_.index_state.last_index_embedded,
                                                               sh_.index_state.last_index_found, sh_.db_count);
            const uint64_t embedded = shown.embedded;
            const uint64_t total = shown.total;

            const std::wstring num_a = format_count(embedded);
            const std::wstring num_b = L" / " + format_count(total);
            const float wa = cv.measure(num_a, Font::Hero);
            const float wb = cv.measure(num_b, Font::Title);

            std::wstring status_txt = stopping ? L"Stopping…" : (running ? phase_label(d.phase) : (sh_.index_state.last_error.empty() ? L"Up to date" : L"Error"));
            D2D1_COLOR_F badge_col = running ? color::amber : (sh_.index_state.last_error.empty() ? color::mint : color::error);
            if (!running && d.phase == Phase::Stopped && sh_.index_state.last_error.empty()) {
                status_txt = L"Stopped";
                badge_col = color::amber;
            } else if (!running && sh_.index_state.last_index_unix == 0) {
                status_txt = L"Never indexed";
                badge_col = color::outline;
            }
            const float bw = cv.measure(status_txt, Font::Caption) + 28.0f;
            const uint64_t fail_cnt = running ? d.failed : sh_.failed_files.size();
            const std::wstring ftxt = format_count(fail_cnt) + L" failed";
            const float fw = cv.measure(ftxt, Font::Caption) + 18.0f;

            std::wstring sub;
            if (running) {
                if (d.rate > 0.05f) {
                    wchar_t rbuf[64];
                    swprintf_s(rbuf, L"%.1f files/s", d.rate);
                    sub += rbuf;
                    if (d.todo > d.processed) {
                        const uint64_t rem_ms = static_cast<uint64_t>((d.todo - d.processed) / d.rate * 1000.0f);
                        sub += L"  •  ETA " + format_duration_ms(rem_ms);
                    }
                }
                if (d.device[0]) {
                    if (!sub.empty()) sub += L"  •  ";
                    sub += d.device;
                }
                if (d.batch) sub += L" (batch " + std::to_wstring(d.batch) + L")";
            } else if (!sh_.index_state.last_error.empty()) {
                sub = to_wide(sh_.index_state.last_error);
            } else {
                sub = L"Last indexed " + format_ago(sh_.index_state.last_index_unix);
            }

            float pct = total > 0 ? static_cast<float>(embedded) / static_cast<float>(total) : (running ? 0.05f : 0.0f);
            pct = std::clamp(pct, 0.0f, 1.0f);
            wchar_t pbuf[32];
            swprintf_s(pbuf, L"%.1f%% completed", pct * 100.0f);
            const std::wstring cur = stopping ? L"Stopping indexer — finishing current batch…"
                                   : (running && d.current[0] ? (std::wstring{L"Processing: "} + d.current)
                                   : (running && d.message[0] ? std::wstring{d.message} : L"On-demand scanner idle — zero background GPU/RAM usage"));
            const std::wstring db_info = L"Qdrant collection: " + to_wide(sh_.settings.model_name) +
                                         L"  •  127.0.0.1:" + std::to_wstring(sh_.settings.qdrant_http_port);

            const wchar_t* label = L"LOCAL MEDIA EMBEDDING STATUS";
            const float label_h = text_height(cv, label, Font::Label, rw, 16.0f);

            Flow numbers{0.0f, 0.0f};
            numbers.add(wa + 4.0f, 40.0f, [&](D2D1_RECT_F r) {
                cv.text(num_a, rect(r.left, r.top, width(r), 40.0f), Font::Hero, color::on_surface);
            });
            numbers.add(wb + 8.0f, 40.0f, [&](D2D1_RECT_F r) {
                cv.text(num_b, rect(r.left, r.top + 8.0f, width(r), 32.0f), Font::Title, color::outline);
            });
            numbers.add(cv.measure(L"files embedded", Font::Body) + 4.0f, 40.0f, [&](D2D1_RECT_F r) {
                cv.text(L"files embedded", rect(r.left, r.top + 12.0f, width(r), 28.0f), Font::Body, color::on_surface_variant);
            });

            const wchar_t* start_label = stopping ? L"Stopping…" : (running ? L"Stop Indexing" : L"Start Indexing");
            Flow headline{16.0f, 8.0f};
            headline.add_group(numbers, rw, true);
            headline.add(button_width(cv, start_label, true, 156.0f), 40.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, running ? glyph::pause : glyph::play, start_label, IdToggleIndex, 0, running ? 2 : 1);
            });

            Flow status{12.0f, 6.0f};
            status.add(bw, 22.0f, [&](D2D1_RECT_F br) {
                cv.fill_round(br, 11.0f, with_alpha(badge_col, 0.15f));
                cv.fill_circle({br.left + 11.0f, br.top + 11.0f}, 3.5f, badge_col);
                cv.text(status_txt, rect(br.left + 20.0f, br.top, bw - 22.0f, 22.0f), Font::Caption, badge_col);
            });
            if (fail_cnt > 0) {
                status.add(fw, 22.0f, [&](D2D1_RECT_F fr) {
                    cv.fill_round(fr, 11.0f, with_alpha(color::error, 0.14f));
                    cv.text(ftxt, fr, Font::Caption, color::error, Align::Center);
                });
            }
            flow_text(cv, status, sub, Font::Caption, color::on_surface_variant, rw, 22.0f);

            Flow progress_text{8.0f, 4.0f};
            flow_text(cv, progress_text, pbuf, Font::Caption, color::outline, rw, 20.0f);
            flow_text(cv, progress_text, cur, Font::MonoSmall, stopping ? color::amber : color::on_surface_variant, rw, 20.0f, true, Align::Right);

            Flow database{8.0f, 8.0f};
            flow_text(cv, database, db_info, Font::Caption, color::outline, rw, 32.0f, true);
            database.add(button_width(cv, L"Open Qdrant Web UI", true, 166.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::open, L"Open Qdrant Web UI", IdOpenQdrantUi);
            });
            database.add(button_width(cv, L"Clear Database", true, 130.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::del, L"Clear Database", IdClearDatabase, 0, 2);
            });

            const float headline_y = y + 16.0f + label_h + 2.0f;
            const float status_y = headline_y + headline.height(rw) + 8.0f;
            const float track_y = status_y + status.height(rw) + 12.0f;
            const float progress_y = track_y + 16.0f;
            const float database_line_y = progress_y + progress_text.height(rw) + 10.0f;
            const float database_y = database_line_y + 11.0f;
            const float h = database_y + database.height(rw) + 11.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            cv.text_wrap(label, rect(lx, y + 16.0f, rw, label_h), Font::Label, color::primary);
            headline.draw(lx, headline_y, rw);
            status.draw(lx, status_y, rw);

            const D2D1_RECT_F track = rect(lx, track_y, rw, 8.0f);
            cv.fill_round(track, 4.0f, color::surface_highest);
            if (pct > 0.0f) {
                cv.fill_round(rect(track.left, track.top, std::max(8.0f, width(track) * pct), 8.0f), 4.0f, running ? color::amber : color::primary);
            }
            progress_text.draw(lx, progress_y, rw);

            cv.hline(lx, lx + rw, database_line_y, with_alpha(color::outline_variant, 0.5f));
            database.draw(lx, database_y, rw);
            y = next_y;
        }

        // 1b. Failed files card (shown when decode/embed failures exist)
        if (!sh_.failed_files.empty()) {
            const float lx = x + 22.0f, rw = w - 44.0f;
            const size_t show_n = std::min<size_t>(sh_.failed_files.size(), 6);
            const bool stacked = rw < 560.0f;
            const float path_x = 32.0f;
            const float path_w = stacked ? rw - 40.0f : rw * 0.64f - path_x - 4.0f;
            const float reason_x = stacked ? path_x : rw * 0.64f;
            const float reason_w = stacked ? rw - 40.0f : rw * 0.36f - 10.0f;

            const std::wstring title = L"Failed Media Files (" + format_count(sh_.failed_files.size()) + L")";
            const CardHeader hd = make_header(cv, rw, title, color::error,
                                              L"Skipped until modified — click a file to reveal in Explorer, or retry all", 266.0f, 32.0f);

            struct FailedRow {
                std::wstring label;
                float y;
                float h;
                float content_h;
                float path_h;
            };
            std::vector<FailedRow> rows;
            float cy = y + 16.0f + hd.h + 10.0f;
            for (size_t i = 0; i < show_n; ++i) {
                const auto& ff = sh_.failed_files[i];
                FailedRow row{ff.path.empty() ? (L"ID " + std::to_wstring(ff.id)) : ff.path, cy, 0.0f, 0.0f, 0.0f};
                row.path_h = cv.measure_height(row.label, Font::MonoSmall, path_w);
                const float reason_h = cv.measure_height(ff.reason, Font::Caption, reason_w);
                row.content_h = stacked ? row.path_h + (reason_h > 0.0f ? 2.0f + reason_h : 0.0f) : std::max(row.path_h, reason_h);
                row.h = std::max(32.0f, row.content_h + 12.0f);
                rows.push_back(std::move(row));
                cy += rows.back().h + 4.0f;
            }
            const bool has_more = sh_.failed_files.size() > show_n;
            const std::wstring more = has_more
                ? L"+ " + format_count(sh_.failed_files.size() - show_n) + L" more in " + failed_path(sh_.settings.model_name).filename().wstring()
                : std::wstring{};
            const float more_h = has_more ? text_height(cv, more, Font::Caption, rw, 20.0f) : 0.0f;
            const float list_bottom = has_more ? cy + more_h : cy - 4.0f;
            const float h = list_bottom + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_button(cv, rect(side.left, side.top, 126.0f, 32.0f), glyph::open, L"Open Log File", IdOpenFailedLog);
            draw_button(cv, rect(side.left + 134.0f, side.top, 132.0f, 32.0f), glyph::refresh, L"Retry Failed", IdClearFailed);

            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& ff = sh_.failed_files[i];
                const FailedRow& row = rows[i];
                const D2D1_RECT_F ir = rect(lx, row.y, rw, row.h);
                const bool hov = hovered(IdFailedRow, static_cast<int>(i));
                cv.fill_round(ir, 8.0f, hov ? color::surface_high : color::surface_low);
                cv.icon(glyph::warning, rect(ir.left + 8.0f, ir.top, 20.0f, row.h), color::error, Font::IconSmall);
                const float content_top = ir.top + (row.h - row.content_h) * 0.5f;
                cv.text_wrap(row.label, rect(ir.left + path_x, content_top, path_w, row.path_h), Font::MonoSmall, color::on_surface);
                const float reason_y = stacked ? content_top + row.path_h + 2.0f : ir.top;
                const float reason_h = stacked ? row.content_h - row.path_h - 2.0f : row.h;
                cv.text_wrap(ff.reason, rect(ir.left + reason_x, reason_y, reason_w, reason_h), Font::Caption, color::outline, stacked ? Align::Left : Align::Right);
                if (!ff.path.empty()) cv.hit(ir, IdFailedRow, static_cast<int>(i));
            }
            if (has_more) {
                cv.text_wrap(more, rect(lx, cy, rw, more_h), Font::Caption, color::outline);
            }
            y = next_y;
        }

        // 2. Indexing Scope Card
        {
            const bool custom = !sh_.settings.whole_pc;
            const size_t inc_n = sh_.settings.include_dirs.size();
            const float lx = x + 22.0f, rw = w - 44.0f;

            const CardHeader hd = make_header(cv, rw, L"Indexing Scope", color::on_surface,
                                              L"Scan the whole PC with Voidtools Everything, or restrict indexing to specific folders");

            struct Radio {
                bool on;
                const wchar_t* title;
                const wchar_t* desc;
                int id;
            };
            const Radio radios[2] = {
                {sh_.settings.whole_pc, L"Include the whole PC", L"All local fixed drives (minus exclusions)", IdScopeAll},
                {!sh_.settings.whole_pc, L"Specify directories to include", L"Only scan the folders listed below", IdScopeCustom},
            };
            constexpr float radio_min_w = 300.0f;
            const float radio_h = std::max(radio_height(cv, radio_min_w, radios[0].title, radios[0].desc),
                                           radio_height(cv, radio_min_w, radios[1].title, radios[1].desc));
            Flow radio_flow{12.0f, 8.0f};
            for (const Radio& radio : radios) {
                radio_flow.add(radio_min_w, radio_h, [&, radio](D2D1_RECT_F r) {
                    draw_radio(cv, r, radio.on, radio.title, radio.desc, radio.id);
                }, true);
            }

            Flow include_head{8.0f, 8.0f};
            flow_text(cv, include_head, L"INCLUDED FOLDERS", Font::Label, color::primary, rw, 30.0f, true);
            include_head.add(button_width(cv, L"Add Directory", true, 134.0f), 30.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::add, L"Add Directory", IdAddInclude);
            });

            const wchar_t* empty_msg = L"No folders added yet — click Add Directory to choose folders to scan";
            const float empty_h = std::max(34.0f, 14.0f + cv.measure_height(empty_msg, Font::Small, rw - 24.0f));
            std::vector<std::wstring> include_paths;
            std::vector<float> include_y, include_h;

            float cy = y + 16.0f + hd.h + 10.0f;
            const float radios_y = cy;
            cy += radio_flow.height(rw) + 12.0f;
            const float include_head_y = cy;
            float empty_y = 0.0f;
            float include_error_y = 0.0f;
            float include_error_h = 0.0f;
            if (custom) {
                cy += include_head.height(rw) + 8.0f;
                if (!include_error_.empty()) {
                    include_error_y = cy;
                    include_error_h = error_row_height(cv, include_error_, rw);
                    cy += include_error_h + 8.0f;
                }
                if (inc_n == 0) {
                    empty_y = cy;
                    cy += empty_h + 4.0f;
                }
                for (size_t i = 0; i < inc_n; ++i) {
                    include_paths.push_back(to_wide(sh_.settings.include_dirs[i]));
                    include_y.push_back(cy);
                    include_h.push_back(mono_row_height(cv, include_paths.back(), rw - 72.0f, 34.0f));
                    cy += include_h.back() + 4.0f;
                }
                cy += 8.0f;
            }
            const float line_y = cy;
            cy += 12.0f;

            const std::wstring min_lbl = L"Min size: " + std::to_wstring(sh_.settings.min_file_kb) + L" KB";
            Flow media{8.0f, 8.0f};
            media.add(button_width(cv, L"Images", true, 108.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::photo, L"Images", IdToggleImages, 0, sh_.settings.index_images ? 1 : 0);
            });
            media.add(button_width(cv, L"Videos", true, 108.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::video, L"Videos", IdToggleVideos, 0, sh_.settings.index_videos ? 1 : 0);
            });
            media.add(0.0f, 32.0f, [](D2D1_RECT_F) {}, true);
            media.add(button_width(cv, min_lbl, false, 136.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, 0, min_lbl, IdMinKbStep);
            });
            const float media_y = cy;
            cy += media.height(rw) + 10.0f;

            const wchar_t* limits_note = L"Click Min size to cycle minimum file size threshold. Videos are embedded by averaging keyframes.";
            const float limits_note_h = text_height(cv, limits_note, Font::Caption, rw, 18.0f);
            const float limits_note_y = cy;
            const float h = cy + limits_note_h + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            draw_header(cv, lx, y, rw, hd);
            radio_flow.draw(lx, radios_y, rw);

            if (custom) {
                include_head.draw(lx, include_head_y, rw);
                if (!include_error_.empty()) {
                    draw_error_row(cv, include_error_, lx, include_error_y, rw, include_error_h);
                }
                if (inc_n == 0) {
                    const D2D1_RECT_F er = rect(lx, empty_y, rw, empty_h);
                    cv.fill_round(er, 8.0f, color::surface_low);
                    text_middle(cv, empty_msg, inset(er, 12.0f, 0.0f), Font::Small, color::outline);
                }
                for (size_t i = 0; i < inc_n; ++i) {
                    const D2D1_RECT_F ir = rect(lx, include_y[i], rw, include_h[i]);
                    cv.fill_round(ir, 8.0f, color::surface_low);
                    cv.icon(glyph::folder, rect(ir.left + 8.0f, ir.top, 22.0f, include_h[i]), color::primary, Font::IconSmall);
                    text_middle(cv, include_paths[i], rect(ir.left + 34.0f, ir.top, rw - 72.0f, include_h[i]), Font::MonoSmall, color::on_surface);
                    draw_button(cv, rect(ir.right - 32.0f, ir.top + (include_h[i] - 28.0f) * 0.5f, 28.0f, 28.0f), glyph::remove, L"", IdRemoveInclude, static_cast<int>(i), 3);
                }
            }

            cv.hline(lx, lx + rw, line_y, with_alpha(color::outline_variant, 0.5f));
            media.draw(lx, media_y, rw);
            cv.text_wrap(limits_note, rect(lx, limits_note_y, rw, limits_note_h), Font::Caption, color::outline);
            y = next_y;
        }

        // 3. Excluded Directories Card (always visible)
        {
            const auto& locked = locked_excludes();
            const bool show_exclude_error = !exclude_error_.empty() && exclude_field_.text == exclude_error_for_text_;
            const float lx = x + 22.0f, rw = w - 44.0f;

            const CardHeader hd = make_header(cv, rw, L"Excluded Directories & Patterns", color::on_surface,
                                              L"Skipped in both Whole PC and Custom Folder modes", 142.0f, 32.0f);

            Flow input{8.0f, 8.0f};
            input.add(180.0f, 34.0f, [&](D2D1_RECT_F fr) {
                cv.fill_round(fr, 8.0f, color::surface_low);
                cv.stroke_round(fr, 8.0f, active_field_ == &exclude_field_ ? color::primary : color::outline_variant);
                exclude_field_.draw(cv, inset(fr, 10.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                cv.hit(fr, IdExcludeField);
            }, true);
            input.add(button_width(cv, L"Add Rule", true, 100.0f), 34.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::add, L"Add Rule", IdAddExcludePattern);
            });

            struct ExcludeRow {
                std::wstring text;
                std::wstring tag;
                bool locked;
                int index;
                float y;
                float h;
            };
            std::vector<ExcludeRow> rows;
            rows.reserve(locked.size() + sh_.settings.exclude_dirs.size());
            for (const auto& sys : locked) {
                rows.push_back({sys, L"System", true, 0, 0.0f, 0.0f});
            }
            for (size_t i = 0; i < sh_.settings.exclude_dirs.size(); ++i) {
                const std::wstring wex = to_wide(sh_.settings.exclude_dirs[i]);
                const bool is_pat = wex.contains(L'*') || !(wex.size() >= 2 && wex[1] == L':');
                rows.push_back({wex, is_pat ? L"Pattern" : L"Folder", false, static_cast<int>(i), 0.0f, 0.0f});
            }

            const float input_y = y + 16.0f + hd.h + 10.0f;
            const float input_h = input.height(rw);
            const float error_y = input_y + input_h + 4.0f;
            const float error_h = show_exclude_error ? error_row_height(cv, exclude_error_, rw) : 0.0f;
            float cy = input_y + input_h + 10.0f + (show_exclude_error ? error_h + 2.0f : 0.0f);
            for (ExcludeRow& row : rows) {
                row.y = cy;
                row.h = mono_row_height(cv, row.text, rw - 120.0f, 32.0f);
                cy += row.h + 4.0f;
            }
            const float h = (rows.empty() ? cy : cy - 4.0f) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_button(cv, side, glyph::folder, L"Add Folder…", IdAddExcludeFolder);

            input.draw(lx, input_y, rw);
            if (show_exclude_error) {
                draw_error_row(cv, exclude_error_, lx, error_y, rw, error_h);
            }

            for (const ExcludeRow& row : rows) {
                const D2D1_RECT_F ir = rect(lx, row.y, rw, row.h);
                cv.fill_round(ir, 8.0f, row.locked ? with_alpha(color::surface_lowest, 0.7f) : color::surface_low);
                cv.icon(glyph::block, rect(ir.left + 8.0f, ir.top, 20.0f, row.h), row.locked ? color::outline : color::error, Font::IconSmall);
                text_middle(cv, row.text, rect(ir.left + 32.0f, ir.top, rw - 120.0f, row.h), Font::MonoSmall, row.locked ? color::outline : color::on_surface);
                cv.text(row.tag, rect(ir.right - 92.0f, ir.top, 56.0f, row.h), Font::Caption, row.locked ? color::outline : color::on_surface_variant, Align::Right);
                if (row.locked) {
                    cv.icon(glyph::lock, rect(ir.right - 30.0f, ir.top, 24.0f, row.h), color::outline, Font::IconSmall);
                } else {
                    draw_button(cv, rect(ir.right - 30.0f, ir.top + (row.h - 28.0f) * 0.5f, 28.0f, 28.0f), glyph::remove, L"", IdRemoveExclude, row.index, 3);
                }
            }
            y = next_y;
        }
        return y;
    }

    // ------------------------------------------------------------------------------------------
    // Tab 2: Application & Venv Settings
    // ------------------------------------------------------------------------------------------

    float draw_tab_settings(Canvas& cv, float x, float y, float w) {
        // 1. Keybind & Pill behaviour card
        {
            const float lx = x + 22.0f, rw = w - 44.0f;

            const CardHeader hd = make_header(cv, rw, L"Spotlight Invocation Keybind", color::on_surface,
                                              L"Global desktop shortcut registered by the lightweight tray launcher", 44.0f, 24.0f);

            const std::wstring hk = recording_hotkey_ ? L"Press shortcut…" : format_hotkey(sh_.settings.hotkey_mods, sh_.settings.hotkey_vk);
            const wchar_t* record_label = recording_hotkey_ ? L"Cancel Recording" : L"Record Keybind";
            Flow keys{8.0f, 8.0f};
            keys.add(240.0f, 40.0f, [&](D2D1_RECT_F kr) {
                cv.fill_round(kr, 10.0f, color::surface_lowest);
                cv.stroke_round(kr, 10.0f, recording_hotkey_ ? color::amber : color::outline_variant, recording_hotkey_ ? 1.5f : 1.0f);
                cv.text(hk, kr, Font::BodySemi, recording_hotkey_ ? color::amber : color::primary, Align::Center);
            });
            keys.add(button_width(cv, record_label, true, 160.0f), 40.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::keyboard, record_label, IdRecordHotkey, 0, recording_hotkey_ ? 1 : 0);
            });
            keys.add(button_width(cv, L"Reset (Alt+Space)", false, 118.0f), 40.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, 0, L"Reset (Alt+Space)", IdResetHotkey, 0, 3);
            });

            Flow startup{12.0f, 0.0f};
            flow_text(cv, startup, L"Open on PC startup", Font::Body, color::on_surface, rw, 32.0f);
            startup.add(44.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_switch(cv, r.right, (r.top + r.bottom) * 0.5f, sh_.settings.launch_on_startup, IdToggleStartup);
            });
            Flow hide_on_blur{12.0f, 0.0f};
            flow_text(cv, hide_on_blur, L"Hide pill when clicking outside", Font::Body, color::on_surface, rw, 32.0f);
            hide_on_blur.add(44.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_switch(cv, r.right, (r.top + r.bottom) * 0.5f, sh_.settings.hide_on_focus_loss, IdToggleHideFocus);
            });

            const std::wstring idle_lbl = sh_.settings.ui_idle_timeout_s < 0
                ? L"UI process timeout: Never"
                : (sh_.settings.ui_idle_timeout_s == 0
                    ? L"UI process timeout: 0s (Immediate)"
                    : L"UI process timeout: " + std::to_wstring(sh_.settings.ui_idle_timeout_s) + L"s");
            Flow options{24.0f, 10.0f};
            options.add_group(startup, rw);
            options.add_group(hide_on_blur, rw);
            options.add(0.0f, 32.0f, [](D2D1_RECT_F) {}, true);
            options.add(button_width(cv, idle_lbl, true, 238.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::clock, idle_lbl, IdIdleStep);
            });

            const bool contain = parse_thumb_fit(sh_.settings.thumb_fit) == ThumbFit::Contain;
            Flow thumbs{8.0f, 8.0f};
            flow_text(cv, thumbs, L"Image thumbnails:", Font::SmallSemi, color::on_surface_variant, rw, 32.0f);
            thumbs.add(button_width(cv, L"Fill tile (crop)", false, 120.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, 0, L"Fill tile (crop)", IdThumbFit, static_cast<int>(ThumbFit::Crop), !contain ? 1 : 0);
            });
            thumbs.add(button_width(cv, L"Whole image (fit)", false, 150.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, 0, L"Whole image (fit)", IdThumbFit, static_cast<int>(ThumbFit::Contain), contain ? 1 : 0);
            });

            // The launcher could not register the shortcut: say so here instead of leaving a dead key
            const bool hotkey_warn = sh_.hotkey_unavailable() && !recording_hotkey_;
            const std::wstring hotkey_msg = format_hotkey(sh_.settings.hotkey_mods, sh_.settings.hotkey_vk) +
                L" is already registered by another app, so it cannot open search. Record a different shortcut or close the app that uses it.";
            const float hotkey_msg_h = hotkey_warn ? error_row_height(cv, hotkey_msg, rw) : 0.0f;

            const float keys_y = y + 16.0f + hd.h + 10.0f;
            const float hotkey_msg_y = keys_y + keys.height(rw) + 8.0f;
            const float options_line_y = keys_y + keys.height(rw) + (hotkey_warn ? 8.0f + hotkey_msg_h : 0.0f) + 14.0f;
            const float options_y = options_line_y + 10.0f;
            const float thumbs_y = options_y + options.height(rw) + 16.0f;
            const float h = thumbs_y + thumbs.height(rw) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_switch(cv, side.right, side.top + 12.0f, sh_.settings.hotkey_enabled, IdToggleHotkey);
            keys.draw(lx, keys_y, rw);
            if (hotkey_warn) draw_error_row(cv, hotkey_msg, lx, hotkey_msg_y, rw, hotkey_msg_h);
            cv.hline(lx, lx + rw, options_line_y, with_alpha(color::outline_variant, 0.5f));
            options.draw(lx, options_y, rw);
            thumbs.draw(lx, thumbs_y, rw);
            y = next_y;
        }

        // 2. Embeddings AI Model & GPU Batching card
        {
            const float lx = x + 22.0f, rw = w - 44.0f;

            const CardHeader hd = make_header(cv, rw, L"Embeddings AI Model & Compute", color::on_surface,
                                              L"Vision weights (.safetensors) and text ONNX / tokenizer directory");

            // Model dir row
            const std::wstring model_dir = to_wide(sh_.settings.model_dir);
            const bool dir_beside = rw - 116.0f >= 220.0f;
            const float dir_w = dir_beside ? rw - 116.0f : rw;
            const float dir_h = mono_row_height(cv, model_dir, dir_w - 20.0f, 32.0f);
            Flow dir_row{8.0f, 8.0f};
            dir_row.add(dir_w, dir_h, [&](D2D1_RECT_F dr) {
                cv.fill_round(dr, 8.0f, color::surface_lowest);
                text_middle(cv, model_dir, inset(dr, 10.0f, 0.0f), Font::MonoSmall, color::on_surface_variant);
            });
            dir_row.add(button_width(cv, L"Browse…", true, 108.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::folder, L"Browse…", IdBrowseModelDir);
            });

            // Model name chips + custom field
            Flow chips{8.0f, 8.0f};
            flow_text(cv, chips, L"Model:", Font::SmallSemi, color::on_surface_variant, rw, 32.0f);
            for (size_t i = 0; i < models_.size() && i < 3; ++i) {
                const std::wstring wm = to_wide(models_[i]);
                const bool on = models_[i] == sh_.settings.model_name;
                chips.add(button_width(cv, wm, false, 0.0f), 32.0f, [&, wm, i, on](D2D1_RECT_F r) {
                    draw_button(cv, r, 0, wm, IdModelChip, static_cast<int>(i), on ? 1 : 0);
                });
            }
            chips.add(140.0f, 32.0f, [&](D2D1_RECT_F mfr) {
                cv.fill_round(mfr, 8.0f, color::surface_low);
                cv.stroke_round(mfr, 8.0f, active_field_ == &model_field_ ? color::primary : color::outline_variant);
                model_field_.draw(cv, inset(mfr, 10.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                cv.hit(mfr, IdModelField);
            }, true);

            // What is on disk for the selected model, and a way to fetch what is missing
            const ModelFiles& mf = model_files_;
            const JobView fetch_view = view(model_job_);
            const auto badge = [&cv](Flow& flow, std::wstring text, D2D1_COLOR_F col) {
                const float bw = cv.measure(text, Font::Caption) + 28.0f;
                flow.add(bw, 22.0f, [&cv, text = std::move(text), col, bw](D2D1_RECT_F br) {
                    cv.fill_round(br, 11.0f, with_alpha(col, 0.15f));
                    cv.fill_circle({br.left + 11.0f, br.top + 11.0f}, 3.5f, col);
                    cv.text(text, rect(br.left + 20.0f, br.top, bw - 22.0f, 22.0f), Font::Caption, col);
                });
            };
            Flow files{8.0f, 6.0f};
            badge(files, mf.weights ? L"Weights  " + format_size(mf.weights_bytes) : L"Weights missing", mf.weights ? color::mint : color::error);
            badge(files, mf.tokenizer ? L"Tokenizer" : L"Tokenizer missing", mf.tokenizer ? color::mint : color::error);
            badge(files, mf.onnx ? L"Text ONNX" : L"Text ONNX not exported yet", mf.onnx ? color::mint : color::amber);
            const auto enc_state = sh_.engine.encoder().state();
            if (enc_state == TextEncoder::State::Ready) badge(files, L"Search model loaded", color::mint);
            else if (enc_state == TextEncoder::State::Exporting) badge(files, L"Exporting text ONNX…", color::primary);
            else if (enc_state == TextEncoder::State::Loading) badge(files, L"Loading search model…", color::primary);
            const std::wstring enc_error = enc_state == TextEncoder::State::Failed ? sh_.engine.encoder().error() : std::wstring{};
            const float enc_error_h = enc_error.empty() ? 0.0f : error_row_height(cv, enc_error, rw);

            const bool show_fetch = !mf.weights || !mf.tokenizer || model_job_.busy;
            repo_field_.placeholder = to_wide(default_repo());
            const std::wstring fetch_label = !model_job_.busy ? L"Download Model"
                : (fetch_view.total > 0 ? L"Cancel  •  " + std::to_wstring(fetch_view.done * 100 / fetch_view.total) + L"%" : L"Cancel");
            Flow fetch{8.0f, 8.0f};
            flow_text(cv, fetch, L"Hugging Face repo:", Font::SmallSemi, color::on_surface_variant, rw, 32.0f);
            fetch.add(200.0f, 32.0f, [&](D2D1_RECT_F fr) {
                cv.fill_round(fr, 8.0f, color::surface_low);
                cv.stroke_round(fr, 8.0f, active_field_ == &repo_field_ ? color::primary : color::outline_variant);
                repo_field_.draw(cv, inset(fr, 10.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                cv.hit(fr, IdRepoField);
            }, true);
            fetch.add(button_width(cv, fetch_label, true, 170.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, model_job_.busy ? glyph::close : glyph::download, fetch_label, IdDownloadModel, 0, model_job_.busy ? 2 : 1);
            });
            std::wstring fetch_note = fetch_view.log;
            if (model_job_.busy && fetch_view.total > 0) {
                fetch_note += L"  " + format_size(fetch_view.done) + L" / " + format_size(fetch_view.total);
            }
            const float fetch_note_h = fetch_note.empty() ? 0.0f : text_height(cv, fetch_note, Font::Caption, rw, 18.0f);

            // Device & VRAM Batch size
            const bool is_cpu = sh_.settings.device == "cpu";
            Flow device{6.0f, 0.0f};
            flow_text(cv, device, L"Device:", Font::SmallSemi, color::on_surface_variant, rw, 32.0f);
            device.add(button_width(cv, L"GPU (ROCm / CUDA)", true, 138.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::chip, L"GPU (ROCm / CUDA)", IdDeviceAuto, 0, !is_cpu ? 1 : 0);
            });
            device.add(button_width(cv, L"CPU Only", false, 86.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, 0, L"CPU Only", IdDeviceCpu, 0, is_cpu ? 1 : 0);
            });

            Flow batch{5.0f, 8.0f};
            flow_text(cv, batch, L"Batch size:", Font::SmallSemi, color::on_surface_variant, rw, 32.0f);
            static constexpr uint32_t batches[] = {0, 32, 64, 128, 256};
            for (const uint32_t b : batches) {
                const std::wstring bl = b == 0 ? L"Auto (VRAM)" : std::to_wstring(b);
                batch.add(button_width(cv, bl, false, b == 0 ? 92.0f : 44.0f), 32.0f, [&, b, bl](D2D1_RECT_F r) {
                    draw_button(cv, r, 0, bl, IdBatchPreset, static_cast<int>(b), sh_.settings.batch_size == b ? 1 : 0);
                });
            }
            const bool custom_batch = sh_.settings.batch_size > 0 &&
                sh_.settings.batch_size != 32 && sh_.settings.batch_size != 64 &&
                sh_.settings.batch_size != 128 && sh_.settings.batch_size != 256;
            batch.add(64.0f, 32.0f, [&](D2D1_RECT_F bfr) {
                cv.fill_round(bfr, 8.0f, color::surface_low);
                cv.stroke_round(bfr, 8.0f, (active_field_ == &batch_field_ || custom_batch) ? color::primary : color::outline_variant,
                                (active_field_ == &batch_field_ || custom_batch) ? 1.5f : 1.0f);
                batch_field_.draw(cv, inset(bfr, 8.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                cv.hit(bfr, IdBatchField);
            }, true);

            Flow compute{14.0f, 8.0f};
            compute.add_group(device, rw);
            compute.add_group(batch, rw, true);

            const bool batch_warn = sh_.settings.batch_size >= 128;
            std::wstring batch_note;
            if (batch_warn) {
                batch_note = L"High fixed batch sizes may exceed VRAM and trigger driver resets; OOM automatically halves the batch.";
            } else if (sh_.settings.batch_size == 0) {
                const std::wstring chosen = sh_.index_state.auto_batch > 0
                    ? L"Auto chose " + std::to_wstring(sh_.index_state.auto_batch) + L" on the last run."
                    : L"Auto measures the fastest batch size on first index.";
                batch_note = chosen + L" Backs off automatically on out-of-memory.";
            } else {
                batch_note = L"Auto probes free VRAM on startup and backs off automatically on out-of-memory.";
            }
            // The measured size is kept until the model or GPU changes; this forgets it so the next run measures again
            const bool can_reset_batch = sh_.settings.batch_size == 0 && sh_.index_state.auto_batch > 0;
            const float note_x = batch_warn ? 22.0f : 0.0f;
            Flow note{10.0f, 4.0f};
            flow_text(cv, note, batch_note, Font::Caption, batch_warn ? color::amber : color::outline, rw - note_x, 24.0f, true);
            if (can_reset_batch) {
                note.add(button_width(cv, L"Reset learnt size", false, 0.0f), 24.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, r, 0, L"Reset learnt size", IdResetAutoBatch);
                });
            }
            const float note_h = note.height(rw - note_x);

            // Custom vision script override
            const std::wstring scr = sh_.settings.vision_script.empty() ? L"Bundled (python\\usyseek_vision.py)" : to_wide(sh_.settings.vision_script);
            const float script_label_w = cv.measure(L"Vision script:", Font::SmallSemi) + 4.0f;
            const float custom_w = button_width(cv, L"Custom…", true, 96.0f);
            const float reset_w = button_width(cv, L"Reset", false, 58.0f);
            const float beside_box_w = rw - script_label_w - custom_w - reset_w - 24.0f;
            const bool script_beside = beside_box_w >= 200.0f;
            const float script_box_w = script_beside ? beside_box_w : rw;
            const float script_box_h = mono_row_height(cv, scr, script_box_w - 20.0f, 30.0f);
            Flow script{8.0f, 8.0f};
            flow_text(cv, script, L"Vision script:", Font::SmallSemi, color::on_surface_variant, rw, 30.0f);
            script.add(script_box_w, script_box_h, [&](D2D1_RECT_F sr) {
                cv.fill_round(sr, 8.0f, color::surface_lowest);
                text_middle(cv, scr, inset(sr, 10.0f, 0.0f), Font::MonoSmall, color::on_surface_variant);
            });
            script.add(custom_w, 30.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::code, L"Custom…", IdBrowseScript);
            });
            script.add(reset_w, 30.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, 0, L"Reset", IdClearScript, 0, 3);
            });

            const float dir_y = y + 16.0f + hd.h + 10.0f;
            const float chips_y = dir_y + dir_row.height(rw) + 10.0f;
            const float files_y = chips_y + chips.height(rw) + 10.0f;
            const float enc_error_y = files_y + files.height(rw) + 6.0f;
            const float fetch_y = enc_error_y + (enc_error.empty() ? 0.0f : enc_error_h + 6.0f) + 2.0f;
            const float fetch_note_y = fetch_y + (show_fetch ? fetch.height(rw) + 6.0f : 0.0f);
            const float compute_y = fetch_note_y + (fetch_note.empty() ? 0.0f : fetch_note_h + 6.0f) + 6.0f;
            const float note_y = compute_y + compute.height(rw) + 6.0f;
            const float script_y = note_y + note_h + 8.0f;
            const float h = script_y + script.height(rw) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            draw_header(cv, lx, y, rw, hd);
            dir_row.draw(lx, dir_y, rw);
            chips.draw(lx, chips_y, rw);
            files.draw(lx, files_y, rw);
            if (!enc_error.empty()) draw_error_row(cv, enc_error, lx, enc_error_y, rw, enc_error_h);
            if (show_fetch) fetch.draw(lx, fetch_y, rw);
            if (!fetch_note.empty()) {
                cv.text_wrap(fetch_note, rect(lx, fetch_note_y, rw, fetch_note_h), Font::Caption, fetch_view.failed ? color::error : color::outline);
            }
            compute.draw(lx, compute_y, rw);
            if (batch_warn) {
                cv.icon(glyph::warning, rect(lx, note_y + 2.0f, 18.0f, 20.0f), color::amber, Font::IconSmall);
            }
            note.draw(lx + note_x, note_y, rw - note_x);
            script.draw(lx, script_y, rw);
            y = next_y;
        }

        // 3. Python Environment Card (.venv or portable NuGet Python)
        {
            const JobView venv_view = view(venv_job_);
            const std::wstring& venv_log = venv_view.log;
            const bool venv_busy = venv_job_.busy;
            const bool has_log = !venv_log.empty();
            const float lx = x + 22.0f, rw = w - 44.0f;

            std::error_code ec;
            const bool ver_ok = venv_.exists && fs::exists(venv_.python_exe, ec);
            const std::wstring ready_txt = venv_.portable ? L"Portable Ready" : L".venv Ready";
            const std::wstring st_txt = venv_busy ? L"Running task…" : (ver_ok ? ready_txt : (venv_.exists ? L"Missing python.exe" : L"Not found"));
            const D2D1_COLOR_F st_col = venv_busy ? color::amber : (ver_ok ? color::mint : color::error);
            const float sw = cv.measure(st_txt, Font::Caption) + 26.0f;

            const CardHeader hd = make_header(cv, rw, L"Python Environment", color::on_surface,
                                              L"Any Python 3.10+ .venv or portable Python folder with PyTorch & open-clip (runs out-of-process)", sw, 24.0f);

            // Details box
            const std::wstring env_path = to_wide(sh_.settings.venv_dir);
            const std::wstring py_info = venv_.exists
                ? (L"Python " + venv_.version + (venv_.portable ? L"  (portable: " : L"  (venv of ") + venv_.home + L")")
                : L"No pyvenv.cfg or python.exe in selected folder";
            const std::wstring pkgs = venv_pkgs_.empty() ? L"—" : venv_pkgs_;
            const float value_w = rw - 120.0f;
            const bool select_beside = value_w - 92.0f >= 150.0f;
            const float env_w = select_beside ? value_w - 92.0f : value_w;
            const float env_text_h = text_height(cv, env_path, Font::MonoSmall, env_w, 22.0f);
            const float env_h = select_beside ? std::max(24.0f, env_text_h) : env_text_h + 4.0f + 24.0f;
            const float py_h = text_height(cv, py_info, Font::Small, value_w, 22.0f);
            const float pkgs_h = text_height(cv, pkgs, Font::Caption, value_w, 22.0f);
            const float box_h = 8.0f + env_h + 2.0f + py_h + 2.0f + pkgs_h + 6.0f;

            // Portable Python (NuGet) row
            static constexpr const char* versions[4] = {"3.14", "3.13", "3.12", "3.11"};
            const std::wstring dl_label = venv_view.total > 0 && venv_busy
                ? (L"Downloading " + std::to_wstring(venv_view.done * 100 / venv_view.total) + L"%")
                : L"Download & Use";
            Flow portable{8.0f, 8.0f};
            flow_text(cv, portable, L"Portable Python:", Font::SmallSemi, color::on_surface_variant, rw, 30.0f);
            for (int i = 0; i < 4; ++i) {
                const bool on = sh_.settings.python_version == versions[i];
                portable.add(52.0f, 30.0f, [&, i, on](D2D1_RECT_F r) {
                    draw_button(cv, r, 0, to_wide(versions[i]), IdPyVerChip, i, on ? 1 : 0);
                });
            }
            portable.add(78.0f, 30.0f, [&](D2D1_RECT_F pvr) {
                cv.fill_round(pvr, 8.0f, color::surface_low);
                cv.stroke_round(pvr, 8.0f, active_field_ == &py_ver_field_ ? color::primary : color::outline_variant);
                py_ver_field_.draw(cv, inset(pvr, 8.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                cv.hit(pvr, IdPyVerField);
            });
            portable.add(0.0f, 30.0f, [](D2D1_RECT_F) {}, true);
            portable.add(button_width(cv, dl_label, true, 196.0f), 30.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::download, dl_label, IdDownloadNugetPy, 0, 1);
            });

            // Torch installer backend row
            const char* backends[4] = {"rocm", "cuda", "cpu", "manual"};
            const wchar_t* blabels[4] = {L"AMD ROCm", L"NVIDIA CUDA", L"CPU", L"Manual"};
            Flow backend{8.0f, 8.0f};
            flow_text(cv, backend, L"PyTorch stack:", Font::SmallSemi, color::on_surface_variant, rw, 30.0f);
            for (int i = 0; i < 4; ++i) {
                const bool on = sh_.settings.torch_backend == backends[i];
                const wchar_t* blabel = blabels[i];
                backend.add(button_width(cv, blabel, false, i == 3 ? 78.0f : 98.0f), 30.0f, [&, i, on, blabel](D2D1_RECT_F r) {
                    draw_button(cv, r, 0, blabel, IdBackendChip, i, on ? 1 : 0);
                });
            }
            if (sh_.settings.torch_backend == "rocm") {
                Flow arch{4.0f, 0.0f};
                flow_text(cv, arch, L"Arch:", Font::Small, color::outline, rw, 30.0f);
                arch.add(86.0f, 30.0f, [&](D2D1_RECT_F ar) {
                    cv.fill_round(ar, 8.0f, color::surface_low);
                    cv.stroke_round(ar, 8.0f, active_field_ == &arch_field_ ? color::primary : color::outline_variant);
                    arch_field_.draw(cv, inset(ar, 8.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                    cv.hit(ar, IdArchField);
                });
                backend.add_group(arch, rw);
            } else if (sh_.settings.torch_backend == "cuda") {
                Flow cuda{4.0f, 0.0f};
                flow_text(cv, cuda, L"CUDA:", Font::Small, color::outline, rw, 30.0f);
                for (size_t i = 0; i < cuda_versions.size(); ++i) {
                    const std::wstring vl = to_wide(cuda_versions[i]);
                    const bool on = sh_.settings.cuda_version == cuda_versions[i];
                    cuda.add(button_width(cv, vl, false, 56.0f), 30.0f, [&, i, vl, on](D2D1_RECT_F r) {
                        draw_button(cv, r, 0, vl, IdCudaPreset, static_cast<int>(i), on ? 1 : 0);
                    });
                }
                const bool custom_cuda = std::ranges::find(cuda_versions, sh_.settings.cuda_version) == cuda_versions.end();
                cuda.add(72.0f, 30.0f, [&, custom_cuda](D2D1_RECT_F cr) {
                    cv.fill_round(cr, 8.0f, color::surface_low);
                    cv.stroke_round(cr, 8.0f, (active_field_ == &cuda_field_ || custom_cuda) ? color::primary : color::outline_variant);
                    cuda_field_.draw(cv, inset(cr, 8.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                    cv.hit(cr, IdCudaField);
                });
                flow_text(cv, cuda, L"Pick the newest version your NVIDIA driver supports.", Font::Caption, color::outline, rw, 30.0f);
                backend.add_group(cuda, rw);
            }
            backend.add(0.0f, 30.0f, [](D2D1_RECT_F) {}, true);
            backend.add(button_width(cv, L"Open CMD in .venv", true, 196.0f), 30.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::terminal, L"Open CMD in .venv", IdOpenTerminal);
            });

            // Optional custom Torch pip args row
            Flow torch{8.0f, 8.0f};
            flow_text(cv, torch, L"Torch pip args:", Font::SmallSemi, color::on_surface_variant, rw, 30.0f);
            torch.add(200.0f, 30.0f, [&](D2D1_RECT_F tfr) {
                cv.fill_round(tfr, 8.0f, color::surface_low);
                cv.stroke_round(tfr, 8.0f, active_field_ == &torch_field_ ? color::primary : color::outline_variant);
                torch_field_.draw(cv, inset(tfr, 10.0f, 2.0f), Font::MonoSmall, color::on_surface, color::outline);
                cv.hit(tfr, IdTorchField);
            }, true);

            // Action buttons
            const float action_w = std::max({button_width(cv, L"Install / Update Packages", true, 0.0f),
                                             button_width(cv, L"Create / Recreate Venv", true, 0.0f),
                                             button_width(cv, L"Delete Venv", true, 0.0f)});
            Flow actions{8.0f, 8.0f};
            actions.add(action_w, 36.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::download, L"Install / Update Packages", IdUpdateVenv);
            }, true);
            actions.add(action_w, 36.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::refresh, L"Create / Recreate Venv", IdRecreateVenv);
            }, true);
            actions.add(action_w, 36.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, r, glyph::del, L"Delete Venv", IdDeleteVenv, 0, 2);
            }, true);

            const float log_h = has_log ? mono_row_height(cv, venv_log, rw - 20.0f, 72.0f) : 0.0f;

            const float box_y = y + 16.0f + hd.h + 10.0f;
            const float portable_y = box_y + box_h + 12.0f;
            const float backend_y = portable_y + portable.height(rw) + 8.0f;
            const float torch_y = backend_y + backend.height(rw) + 8.0f;
            const float actions_y = torch_y + torch.height(rw) + 12.0f;
            const float log_y = actions_y + actions.height(rw) + 10.0f;
            const float h = (has_log ? log_y + log_h : log_y - 10.0f) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            const D2D1_RECT_F sbr = draw_header(cv, lx, y, rw, hd);
            cv.fill_round(sbr, 12.0f, with_alpha(st_col, 0.16f));
            cv.fill_circle({sbr.left + 11.0f, sbr.top + 12.0f}, 3.5f, st_col);
            cv.text(st_txt, rect(sbr.left + 20.0f, sbr.top, sw - 22.0f, 24.0f), Font::Caption, st_col);

            const D2D1_RECT_F box = rect(lx, box_y, rw, box_h);
            cv.fill_round(box, 10.0f, color::surface_lowest);
            cv.stroke_round(box, 10.0f, color::outline_variant);
            float ry = box_y + 8.0f;
            cv.text(L"Env Path:", rect(lx + 14.0f, ry, 96.0f, 22.0f), Font::Caption, color::outline);
            text_middle(cv, env_path, rect(lx + 110.0f, ry, env_w, env_text_h), Font::MonoSmall, color::primary);
            const D2D1_RECT_F select_r = select_beside ? rect(lx + rw - 92.0f, ry - 2.0f, 84.0f, 24.0f)
                                                       : rect(lx + 110.0f, ry + env_text_h + 4.0f, 84.0f, 24.0f);
            draw_button(cv, select_r, glyph::folder, L"Select…", IdBrowseVenv);

            ry += env_h + 2.0f;
            cv.text(L"Interpreter:", rect(lx + 14.0f, ry, 96.0f, 22.0f), Font::Caption, color::outline);
            text_middle(cv, py_info, rect(lx + 110.0f, ry, value_w, py_h), Font::Small, color::on_surface);

            ry += py_h + 2.0f;
            cv.text(L"Packages:", rect(lx + 14.0f, ry, 96.0f, 22.0f), Font::Caption, color::outline);
            text_middle(cv, pkgs, rect(lx + 110.0f, ry, value_w, pkgs_h), Font::Caption, color::on_surface_variant);

            portable.draw(lx, portable_y, rw);
            backend.draw(lx, backend_y, rw);
            torch.draw(lx, torch_y, rw);
            actions.draw(lx, actions_y, rw);

            if (has_log) {
                const D2D1_RECT_F lr = rect(lx, log_y, rw, log_h);
                cv.fill_round(lr, 8.0f, color::surface_lowest);
                cv.text_wrap(venv_log, inset(lr, 10.0f, 6.0f), Font::MonoSmall, color::on_surface_variant);
            }
            y = next_y;
        }
        return y;
    }

    // ------------------------------------------------------------------------------------------
    // Dialogs & Venv actions
    // ------------------------------------------------------------------------------------------

    std::optional<fs::path> pick_path(bool folder, const wchar_t* filter_name = nullptr, const wchar_t* filter_spec = nullptr) {
        wil::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
        dlg->SetOptions(opts);
        if (!folder && filter_name && filter_spec) {
            const COMDLG_FILTERSPEC spec{filter_name, filter_spec};
            dlg->SetFileTypes(1, &spec);
        }
        if (FAILED(dlg->Show(hwnd_))) return std::nullopt;
        wil::com_ptr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return std::nullopt;
        wil::unique_cotaskmem_string raw;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return std::nullopt;
        return fs::path{raw.get()};
    }

    void open_venv_terminal() {
        const fs::path vdir = to_wide(sh_.settings.venv_dir);
        const fs::path act = vdir / L"Scripts" / L"activate.bat";
        std::wstring args;
        if (fs::exists(act)) {
            args = L"/k \"\"" + act.wstring() + L"\"\"";
        } else if (venv_.portable) {
            // Portable Python: put the interpreter and its Scripts folder first on PATH
            args = L"/k \"set \"PATH=" + venv_.home + L";" + venv_.home + L"\\Scripts;%PATH%\"\"";
        }
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.lpVerb = L"open";
        sei.lpFile = L"cmd.exe";
        sei.lpParameters = args.c_str();
        sei.lpDirectory = vdir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    // ------------------------------------------------------------------------------------------
    // Background jobs (downloads, package installs)
    // ------------------------------------------------------------------------------------------

    /// One background task and what it reports. Workers write through `report*`, which also asks for a
    /// repaint; the UI thread draws a snapshot. Nothing a worker does ever posts more than one pending message,
    /// so a chatty download cannot bury mouse and keyboard input the way one message per progress tick did.
    struct Job {
        bool busy = false; // UI thread only
        std::mutex m;
        std::wstring log;  // latest line
        bool failed = false;
        uint64_t done = 0, total = 0; // download progress, total 0 = nothing to show
        std::jthread thread;          // last member: joined before the state it writes to goes away
    };
    struct JobView {
        std::wstring log;
        bool failed = false;
        uint64_t done = 0, total = 0;
    };

    static JobView view(Job& job) {
        const std::lock_guard lk{job.m};
        return {job.log, job.failed, job.done, job.total};
    }

    /// Any thread.
    void request_redraw() {
        if (!redraw_posted_.exchange(true)) PostMessageW(hwnd_, WM_APP_REDRAW, 0, 0);
    }

    void report(Job& job, std::wstring_view line, bool failed = false) {
        std::wstring clean = trim(line);
        if (clean.empty()) return;
        {
            const std::lock_guard lk{job.m};
            job.log = std::move(clean);
            job.failed = failed;
        }
        request_redraw();
    }

    void report_progress(Job& job, uint64_t done, uint64_t total) {
        {
            const std::lock_guard lk{job.m};
            job.done = done;
            job.total = total;
        }
        request_redraw();
    }

    /// Runs `work` on the job's thread, then `finished` back on the UI thread.
    void run_job(Job& job, std::function<void(const std::stop_token&)> work, std::function<void()> finished) {
        if (job.busy) return;
        if (job.thread.joinable()) job.thread.join();
        job.busy = true;
        ++sh_.background_jobs;
        {
            const std::lock_guard lk{job.m};
            job.log = L"Starting…";
            job.failed = false;
            job.done = job.total = 0;
        }
        invalidate();
        job.thread = std::jthread([this, &job, work = std::move(work), finished = std::move(finished)](const std::stop_token& stop) {
            work(stop);
            sh_.call([this, &job, finished] {
                job.busy = false;
                --sh_.background_jobs;
                {
                    const std::lock_guard lk{job.m};
                    job.done = job.total = 0;
                }
                finished();
                if (sh_.visibility_changed) sh_.visibility_changed();
                invalidate();
            });
        });
    }

    struct VenvJobInputs {
        std::string python_version;
        std::string venv_dir;
        std::string torch_pip_args;
        std::string torch_backend;
        std::string rocm_arch;
        std::string cuda_version;
        fs::path base_python;
    };

    VenvJobInputs venv_job_inputs() const {
        return {
            .python_version = sh_.settings.python_version,
            .venv_dir = sh_.settings.venv_dir,
            .torch_pip_args = sh_.settings.torch_pip_args,
            .torch_backend = sh_.settings.torch_backend,
            .rocm_arch = sh_.settings.rocm_arch,
            .cuda_version = sh_.settings.cuda_version,
            .base_python = find_base_python(),
        };
    }

    void run_venv_task(std::function<void(const std::stop_token&)> work) {
        run_job(venv_job_, std::move(work), [this] {
            probe_venv();
            sh_.engine.encoder().retry(); // the text model export needs this environment
        });
    }

    void append_venv_log(std::wstring_view line) { report(venv_job_, line); }

    // ------------------------------------------------------------------------------------------
    // Model download (Hugging Face)
    // ------------------------------------------------------------------------------------------

    /// open_clip weights for the PE models live under timm/<model name>.
    [[nodiscard]] std::string default_repo() const { return "timm/" + sh_.settings.model_name; }

    /// "owner/name", nothing that could change the URL it is pasted into.
    static bool valid_repo(std::string_view repo) {
        const auto slash = repo.find('/');
        if (slash == std::string_view::npos || slash == 0 || slash + 1 >= repo.size() || repo.find('/', slash + 1) != std::string_view::npos) return false;
        if (repo.contains("..")) return false;
        return std::ranges::all_of(repo, [](unsigned char c) { return std::isalnum(c) != 0 || c == '/' || c == '-' || c == '_' || c == '.'; });
    }

    /// Fetches whatever the selected model is missing and stores it under the names the app looks for:
    /// open_clip_model.safetensors -> <model>.safetensors, tokenizer.json -> <model>-tokenizer.json.
    void download_model(const std::stop_token& stop, const std::string& repo, const fs::path& dir, const std::string& name) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::wstring base = L"https://huggingface.co/" + to_wide(repo) + L"/resolve/main/";
        const std::wstring wname = to_wide(name);
        struct Item {
            std::wstring remote;
            fs::path local;
            bool present;
        };
        const Item items[] = {
            {L"tokenizer.json", dir / (wname + L"-tokenizer.json"),
             fs::exists(dir / (wname + L"-tokenizer.json"), ec) || fs::exists(dir / L"tokenizer.json", ec)},
            {L"open_clip_model.safetensors", dir / (wname + L".safetensors"), fs::exists(dir / (wname + L".safetensors"), ec)},
        };
        for (const Item& item : items) {
            if (item.present) continue;
            report(model_job_, L"Downloading " + item.remote + L" from " + to_wide(repo) + L"…");
            const http::Result r = http::download(base + item.remote, item.local,
                                                  [this](uint64_t done, uint64_t total) { report_progress(model_job_, done, total); }, stop);
            report_progress(model_job_, 0, 0);
            if (r.cancelled) {
                report(model_job_, L"Download cancelled — it continues from where it stopped next time.");
                return;
            }
            if (!r.ok) {
                report(model_job_, item.remote + L" from " + to_wide(repo) + L": " + r.error, true);
                return;
            }
        }
        report(model_job_, L"Saved as " + wname + L".safetensors and " + wname + L"-tokenizer.json in " + dir.wstring());
    }

    void start_model_download() {
        if (model_job_.busy) {
            model_job_.thread.request_stop();
            return;
        }
        std::string repo = to_utf8(trim(repo_field_.text));
        // A pasted model page URL works too
        for (const std::string_view prefix : {"https://huggingface.co/", "http://huggingface.co/", "huggingface.co/"}) {
            if (repo.starts_with(prefix)) repo.erase(0, prefix.size());
        }
        while (repo.ends_with('/')) repo.pop_back();
        if (repo.empty()) repo = default_repo();
        if (!valid_repo(repo)) {
            {
                const std::lock_guard lk{model_job_.m};
                model_job_.log = L"Enter the repository as owner/name, for example timm/PE-Core-L-14-336";
                model_job_.failed = true;
            }
            invalidate();
            return;
        }
        run_job(model_job_,
                [this, repo, dir = fs::path{to_wide(sh_.settings.model_dir)}, name = sh_.settings.model_name](const std::stop_token& stop) {
                    download_model(stop, repo, dir, name);
                },
                [this] {
                    scan_models();
                    sh_.engine.encoder().retry();
                });
    }

    void exec_logged(const fs::path& exe, const std::vector<std::wstring>& args) {
        std::wstring full = L"\"" + exe.wstring() + L"\"";
        std::wstring disp = exe.filename().wstring();
        for (const auto& a : args) {
            full += L" \"" + a + L"\"";
            disp += L" " + a;
        }
        append_venv_log(L"> " + disp);
        auto on_line = [this](std::string_view out) { append_venv_log(to_wide(out)); };
        (void)ulp::win::proc::run_process<{.allow_visible_windows = false}>(
            ulp::win::proc::process_settings{
                .commandline = full,
                .on_stdout_line = on_line,
                .on_stderr_line = on_line,
            });
    }

    fs::path find_base_python() const {
        if (!sh_.settings.base_python.empty() && fs::exists(to_wide(sh_.settings.base_python))) {
            return to_wide(sh_.settings.base_python);
        }
        if (venv_.portable && fs::exists(venv_.python_exe)) return venv_.python_exe;
        if (!venv_.home.empty() && fs::exists(fs::path{venv_.home} / L"python.exe")) {
            return fs::path{venv_.home} / L"python.exe";
        }
        const fs::path py_root = fs::path{known_folder(FOLDERID_LocalAppData)} / L"Programs" / L"Python";
        for (const wchar_t* ver : {L"Python314", L"Python313", L"Python312", L"Python311", L"Python310"}) {
            const fs::path candidate = py_root / ver / L"python.exe";
            if (fs::exists(candidate)) return candidate;
        }
        return L"python.exe";
    }

    // ------------------------------------------------------------------------------------------
    // Portable Python from NuGet (python package = full CPython with pip/venv, no installer)
    // ------------------------------------------------------------------------------------------

    static std::vector<int> parse_ver(std::string_view v) {
        std::vector<int> out;
        int cur = 0;
        bool any = false;
        for (char c : v) {
            if (c >= '0' && c <= '9') {
                cur = cur * 10 + (c - '0');
                any = true;
            } else if (c == '.') {
                out.push_back(cur);
                cur = 0;
                any = false;
            } else {
                break;
            }
        }
        if (any) out.push_back(cur);
        return out;
    }

    /// Resolves "3.14" to the newest stable 3.14.x on NuGet; exact versions ("3.12.2") pass through if listed.
    std::optional<std::string> resolve_nuget_python(const std::string& want) {
        append_venv_log(L"Querying NuGet for Python " + to_wide(want) + L"…");
        const auto body = http::get(L"https://api.nuget.org/v3-flatcontainer/python/index.json");
        if (!body) {
            append_venv_log(L"Could not reach api.nuget.org");
            return std::nullopt;
        }
        NugetVersionIndex idx;
        if (glz::read<JSON_READ>(idx, *body)) {
            append_venv_log(L"Unexpected response from NuGet");
            return std::nullopt;
        }
        const bool exact = std::count(want.begin(), want.end(), '.') >= 2 || want.find('-') != std::string::npos;
        if (exact) {
            if (std::find(idx.versions.begin(), idx.versions.end(), want) != idx.versions.end()) return want;
            append_venv_log(L"Python " + to_wide(want) + L" is not published on NuGet");
            return std::nullopt;
        }
        const std::string prefix = want + ".";
        std::optional<std::string> best;
        for (const auto& v : idx.versions) {
            if (!v.starts_with(prefix) || v.contains('-')) continue; // stable only
            if (!best || parse_ver(v) > parse_ver(*best)) best = v;
        }
        if (!best) append_venv_log(L"No stable Python " + to_wide(want) + L".x release on NuGet");
        return best;
    }

    void download_portable_python(const VenvJobInputs& in, const std::stop_token& stop) {
        const std::string want = in.python_version.empty() ? "3.14" : in.python_version;
        const auto ver = resolve_nuget_python(want);
        if (!ver) return;

        const fs::path root = data_dir() / (L"python-" + to_wide(*ver));
        const fs::path pkg = data_dir() / (L"python." + to_wide(*ver) + L".nupkg");
        std::error_code ec;

        if (!fs::exists(root / L"tools" / L"python.exe", ec)) {
            const std::wstring url = L"https://api.nuget.org/v3-flatcontainer/python/" + to_wide(*ver) + L"/python." + to_wide(*ver) + L".nupkg";
            append_venv_log(L"Downloading Python " + to_wide(*ver) + L" from NuGet…");
            const http::Result dl = http::download(url, pkg, [this](uint64_t done, uint64_t total) { report_progress(venv_job_, done, total); }, stop);
            report_progress(venv_job_, 0, 0);
            if (!dl.ok) {
                append_venv_log(dl.cancelled ? L"Download cancelled." : L"Download failed: " + dl.error);
                return;
            }

            // .nupkg is a zip; Windows 10+ ships bsdtar which extracts zips natively
            append_venv_log(L"Extracting…");
            fs::remove_all(root, ec);
            fs::create_directories(root, ec);
            wchar_t sys_dir[MAX_PATH]{};
            GetSystemDirectoryW(sys_dir, MAX_PATH);
            exec_logged(fs::path{sys_dir} / L"tar.exe", {L"-xf", pkg.wstring(), L"-C", root.wstring(), L"tools"});
            fs::remove(pkg, ec);
            if (!fs::exists(root / L"tools" / L"python.exe", ec)) {
                append_venv_log(L"Extraction failed: tools\\python.exe missing");
                return;
            }
        } else {
            append_venv_log(L"Python " + to_wide(*ver) + L" already downloaded, reusing it.");
        }

        // Packages install straight into the portable interpreter (no venv)
        const fs::path py = root / L"tools" / L"python.exe";
        exec_logged(py, {L"-m", L"ensurepip", L"--upgrade"});
        sh_.call([this, root] {
            sh_.settings.venv_dir = to_utf8(root.wstring());
            sh_.save();
            probe_venv();
            invalidate();
        });
        install_packages_into(in, py);
    }

    void install_packages_in_worker(const VenvJobInputs& in) {
        const VenvInfo info = read_venv(to_wide(in.venv_dir));
        if (!info.exists || !fs::exists(info.python_exe)) {
            append_venv_log(L"python.exe not found — create a venv or download a portable Python first.");
            return;
        }
        install_packages_into(in, info.python_exe);
    }

    void install_packages_into(const VenvJobInputs& in, const fs::path& py) {
        exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"pip"});
        const std::wstring custom_args = trim(to_wide(in.torch_pip_args));
        if (!custom_args.empty()) {
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(custom_args.c_str(), &argc);
            std::vector<std::wstring> args = {L"-m", L"pip", L"install", L"--upgrade"};
            for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
            if (argv) LocalFree(argv);
            exec_logged(py, args);
        } else if (in.torch_backend == "manual") {
            append_venv_log(L"Manual PyTorch mode: skipping Torch install, installing requirements.txt…");
        } else if (in.torch_backend == "rocm") {
            const std::wstring arch = to_wide(in.rocm_arch.empty() ? "gfx1030" : in.rocm_arch);
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"--index-url", L"https://stable.repo.amd.com/rocm/whl-next/",
                             L"torch[device-" + arch + L"]", L"torchvision[device-" + arch + L"]", L"rocm[libraries,device-" + arch + L"]"});
        } else if (in.torch_backend == "cuda") {
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"--index-url", to_wide(torch_index_url("cuda", in.cuda_version)),
                             L"torch", L"torchvision"});
        } else {
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"--index-url", to_wide(torch_index_url("cpu", "")),
                             L"torch", L"torchvision"});
        }
        fs::path req = exe_dir() / L"python" / L"requirements.txt";
        if (!fs::exists(req)) {
            if (auto found = find_upwards(L"python\\requirements.txt")) req = *found;
        }
        if (fs::exists(req)) {
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"-r", req.wstring()});
        } else {
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"open-clip-torch", L"timm", L"safetensors", L"onnx", L"onnxscript"});
        }
        append_venv_log(L"Packages up to date.");
    }

    // ------------------------------------------------------------------------------------------
    // Input & scrolling
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] float view_height() const {
        return std::max(1.0f, target_.size().height - tabs_height(target_.size().width) - 1.0f);
    }
    [[nodiscard]] float max_scroll() const {
        return std::max(0.0f, content_h_ - view_height());
    }

    void start_anim() {
        last_anim_ = std::chrono::steady_clock::now();
        if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
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
        if (tab_anim_.step(dt)) moving = true;
        if (sw_hotkey_.step(dt)) moving = true;
        if (sw_startup_.step(dt)) moving = true;
        if (sw_hide_focus_.step(dt)) moving = true;
        return moving;
    }

    void sync_switch_targets(bool animate) noexcept {
        sw_hotkey_.set(sh_.settings.hotkey_enabled ? 1.0f : 0.0f, animate);
        sw_startup_.set(sh_.settings.launch_on_startup ? 1.0f : 0.0f, animate);
        sw_hide_focus_.set(sh_.settings.hide_on_focus_loss ? 1.0f : 0.0f, animate);
    }

    void update_switch_anims() {
        const bool animate = visible() && !sh_.test_mode;
        sync_switch_targets(animate);
        if (animate) start_anim();
    }

    void snap_anims() noexcept {
        scroll_ = target_scroll_;
        tab_anim_.snap(tab_anim_.target);
        sw_hotkey_.snap(sw_hotkey_.target);
        sw_startup_.snap(sw_startup_.target);
        sw_hide_focus_.snap(sw_hide_focus_.target);
    }

    [[nodiscard]] float switch_anim_value(int id, bool fallback_on) const noexcept {
        switch (id) {
            case IdToggleHotkey: return std::clamp(sw_hotkey_.value, 0.0f, 1.0f);
            case IdToggleStartup: return std::clamp(sw_startup_.value, 0.0f, 1.0f);
            case IdToggleHideFocus: return std::clamp(sw_hide_focus_.value, 0.0f, 1.0f);
            default: return fallback_on ? 1.0f : 0.0f;
        }
    }

    void commit_fields() {
        bool changed = false;
        const std::string m = to_utf8(trim(model_field_.text));
        if (!m.empty() && m != sh_.settings.model_name) {
            sh_.settings.model_name = m;
            repo_field_.set(L""); // the repository belonged to the previous model
            scan_models();
            changed = true;
        }
        const std::string a = to_utf8(trim(arch_field_.text));
        if (!a.empty() && a != sh_.settings.rocm_arch) {
            sh_.settings.rocm_arch = a;
            changed = true;
        }
        const std::string cu = to_utf8(trim(cuda_field_.text));
        if (!cu.empty() && cu != sh_.settings.cuda_version) {
            sh_.settings.cuda_version = cu;
            changed = true;
        }
        const std::string tp = to_utf8(trim(torch_field_.text));
        if (tp != sh_.settings.torch_pip_args) {
            sh_.settings.torch_pip_args = tp;
            changed = true;
        }
        const std::string pv = to_utf8(trim(py_ver_field_.text));
        if (!pv.empty() && pv != sh_.settings.python_version) {
            sh_.settings.python_version = pv;
            changed = true;
        }
        const std::wstring bs_txt = trim(batch_field_.text);
        uint32_t new_bs = 0;
        if (!bs_txt.empty() && _wcsicmp(bs_txt.c_str(), L"auto") != 0) {
            try {
                const long v = std::stol(bs_txt);
                if (v > 0) new_bs = static_cast<uint32_t>(std::clamp(v, 1L, 4096L));
            } catch (...) {
                new_bs = sh_.settings.batch_size;
            }
        }
        if (new_bs != sh_.settings.batch_size) {
            sh_.settings.batch_size = new_bs;
            changed = true;
        }
        batch_field_.set(sh_.settings.batch_size > 0 ? std::to_wstring(sh_.settings.batch_size) : L"");
        if (changed) {
            sh_.save();
            if (sh_.settings_changed) sh_.settings_changed();
        }
    }

    bool add_exclude(const std::wstring& v) {
        std::vector<std::wstring> existing = locked_excludes();
        for (const auto& e : sh_.settings.exclude_dirs) existing.push_back(to_wide(e));
        if (is_duplicate_path_rule(v, existing)) {
            exclude_error_ = L"That folder is already excluded";
            exclude_error_for_text_ = exclude_field_.text;
            return false;
        }
        exclude_error_.clear();
        sh_.settings.exclude_dirs.push_back(to_utf8(v));
        sh_.save();
        return true;
    }

    void add_exclude_from_field() {
        const std::wstring v = trim(exclude_field_.text);
        if (v.empty()) return;
        if (add_exclude(v)) exclude_field_.set(L"");
        invalidate();
    }

    void stop_recording() {
        if (!recording_hotkey_) return;
        recording_hotkey_ = false;
        send_cmd_value(find_launcher(), Cmd::SuspendHotkey, uint32_t{0});
        sh_.refresh_hotkey_state();
    }

    /// Hands the saved shortcut to the launcher and learns whether it could be registered.
    void reload_launcher_hotkey() {
        send_to_launcher(Cmd::ReloadSettings);
        sh_.refresh_hotkey_state();
        if (sh_.redraw_all) sh_.redraw_all();
    }

    /// Hit regions are recorded while drawing, so a pending repaint is done before a mouse position is resolved.
    void flush() {
        if (dirty_) render();
    }

    void on_lbutton_down(D2D1_POINT_2F p, bool dbl) {
        flush();
        SetFocus(hwnd_);
        const Hit* h = hit_test(hits_, p);
        const int id = h ? h->id : IdNone;
        const int idx = h ? h->index : 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;

        if (active_field_ && id != IdExcludeField && id != IdModelField && id != IdRepoField && id != IdBatchField && id != IdArchField && id != IdCudaField && id != IdTorchField && id != IdPyVerField) {
            commit_fields();
            active_field_->focused = false;
            active_field_ = nullptr;
        }
        if (recording_hotkey_ && id != IdRecordHotkey) stop_recording();

        auto focus_field = [&](TextField& tf) {
            if (active_field_ && active_field_ != &tf) active_field_->focused = false;
            active_field_ = &tf;
            tf.focused = true;
            tf.caret_on = true;
            Canvas cv{target_.dc(), sh_.fonts, hits_};
            tf.click(cv, p.x, shift);
            if (dbl) tf.select_word_at_caret();
            SetTimer(hwnd_, TIMER_CARET, GetCaretBlinkTime(), nullptr);
        };

        switch (id) {
            case IdTab0: set_tab(0); return;
            case IdTab1: set_tab(1); return;
            case IdToggleIndex:
                if (sh_.indexer_running) {
                    if (!sh_.indexer_stopping) {
                        sh_.indexer_stopping = true;
                        send_to_launcher(Cmd::StopIndex);
                    }
                } else {
                    sh_.indexer_stopping = false;
                    send_to_launcher(Cmd::StartIndex);
                }
                sh_.poll_indexer();
                break;
            case IdOpenQdrantUi:
                send_to_launcher(Cmd::NeedQdrant);
                open_file(L"http://127.0.0.1:" + std::to_wstring(sh_.settings.qdrant_http_port) + L"/dashboard");
                break;
            case IdClearDatabase: {
                const std::wstring msg = L"Clear all indexed embeddings and tags in collection '" +
                                         to_wide(sh_.settings.model_name) + L"'?\n\nYour media files on disk will not be touched.";
                if (MessageBoxW(hwnd_, msg.c_str(), L"Clear Database", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES) {
                    if (sh_.indexer_running) send_to_launcher(Cmd::StopIndex);
                    const std::string coll = sh_.settings.model_name;
                    std::error_code ec;
                    fs::remove(failed_path(coll), ec);
                    fs::remove(state_path(), ec);
                    sh_.index_state = {};
                    sh_.failed_files.clear();
                    sh_.db_count = 0;
                    sh_.tags.clear();
                    sh_.engine.post([this, coll](Db& db) {
                        db.drop_collection(coll);
                        sh_.call([this] {
                            sh_.poll_indexer();
                            sh_.refresh_tags();
                            if (sh_.redraw_all) sh_.redraw_all();
                        });
                    });
                }
                break;
            }
            case IdClearFailed: {
                std::error_code ec;
                fs::remove(failed_path(sh_.settings.model_name), ec);
                sh_.failed_files.clear();
                if (!sh_.indexer_running) {
                    send_to_launcher(Cmd::StartIndex);
                    sh_.poll_indexer(true);
                }
                break;
            }
            case IdOpenFailedLog:
                open_file(failed_path(sh_.settings.model_name).wstring());
                break;
            case IdFailedRow:
                if (idx >= 0 && idx < static_cast<int>(sh_.failed_files.size()) && !sh_.failed_files[idx].path.empty()) {
                    reveal_in_explorer(sh_.failed_files[idx].path);
                }
                break;
            case IdScopeAll:
                sh_.settings.whole_pc = true;
                sh_.save();
                break;
            case IdScopeCustom:
                sh_.settings.whole_pc = false;
                sh_.save();
                break;
            case IdAddInclude:
                if (auto dir = pick_path(true)) {
                    std::vector<std::wstring> existing;
                    existing.reserve(sh_.settings.include_dirs.size());
                    for (const auto& d : sh_.settings.include_dirs) existing.push_back(to_wide(d));
                    if (is_duplicate_path_rule(dir->wstring(), existing)) {
                        include_error_ = L"That folder is already included";
                        break;
                    }
                    include_error_.clear();
                    sh_.settings.include_dirs.push_back(to_utf8(dir->wstring()));
                    sh_.save();
                }
                break;
            case IdRemoveInclude:
                if (idx >= 0 && idx < static_cast<int>(sh_.settings.include_dirs.size())) {
                    sh_.settings.include_dirs.erase(sh_.settings.include_dirs.begin() + idx);
                    include_error_.clear();
                    sh_.save();
                }
                break;
            case IdToggleImages:
                sh_.settings.index_images = !sh_.settings.index_images;
                if (!sh_.settings.index_images && !sh_.settings.index_videos) sh_.settings.index_videos = true;
                sh_.save();
                break;
            case IdToggleVideos:
                sh_.settings.index_videos = !sh_.settings.index_videos;
                if (!sh_.settings.index_images && !sh_.settings.index_videos) sh_.settings.index_images = true;
                sh_.save();
                break;
            case IdMinKbStep: {
                static constexpr uint32_t steps[] = {0, 4, 16, 64, 256, 1024};
                uint32_t next = steps[0];
                for (size_t i = 0; i < std::size(steps); ++i) {
                    if (sh_.settings.min_file_kb == steps[i]) {
                        next = steps[(i + 1) % std::size(steps)];
                        break;
                    }
                }
                sh_.settings.min_file_kb = next;
                sh_.save();
                break;
            }
            case IdAddExcludeFolder:
                if (auto dir = pick_path(true)) add_exclude(dir->wstring());
                break;
            case IdAddExcludePattern: add_exclude_from_field(); break;
            case IdExcludeField: focus_field(exclude_field_); break;
            case IdRemoveExclude:
                if (idx >= 0 && idx < static_cast<int>(sh_.settings.exclude_dirs.size())) {
                    sh_.settings.exclude_dirs.erase(sh_.settings.exclude_dirs.begin() + idx);
                    sh_.save();
                }
                break;
            case IdToggleHotkey:
                sh_.settings.hotkey_enabled = !sh_.settings.hotkey_enabled;
                sh_.save();
                reload_launcher_hotkey();
                update_switch_anims();
                break;
            case IdRecordHotkey:
                if (recording_hotkey_) {
                    stop_recording();
                } else {
                    recording_hotkey_ = true;
                    send_cmd_value(find_launcher(), Cmd::SuspendHotkey, uint32_t{1});
                }
                break;
            case IdResetHotkey:
                sh_.settings.hotkey_mods = MOD_ALT;
                sh_.settings.hotkey_vk = VK_SPACE;
                sh_.settings.hotkey_enabled = true;
                sh_.save();
                reload_launcher_hotkey();
                update_switch_anims();
                break;
            case IdToggleStartup:
                sh_.settings.launch_on_startup = !sh_.settings.launch_on_startup;
                set_startup_enabled(sh_.settings.launch_on_startup);
                sh_.save();
                update_switch_anims();
                break;
            case IdToggleHideFocus:
                sh_.settings.hide_on_focus_loss = !sh_.settings.hide_on_focus_loss;
                sh_.save();
                update_switch_anims();
                break;
            case IdThumbFit:
                sh_.settings.thumb_fit = idx == static_cast<int>(ThumbFit::Contain) ? "contain" : "crop";
                sh_.save();
                if (sh_.redraw_all) sh_.redraw_all();
                break;
            case IdIdleStep: {
                static constexpr int32_t steps[] = {0, 60, 180, 600, 1800, -1};
                int32_t next = 0;
                for (size_t i = 0; i < std::size(steps); ++i) {
                    if (sh_.settings.ui_idle_timeout_s == steps[i]) {
                        next = steps[(i + 1) % std::size(steps)];
                        break;
                    }
                }
                sh_.settings.ui_idle_timeout_s = next;
                sh_.save();
                break;
            }
            case IdBrowseModelDir:
                if (auto dir = pick_path(true)) {
                    sh_.settings.model_dir = to_utf8(dir->wstring());
                    scan_models();
                    sh_.save();
                    if (sh_.settings_changed) sh_.settings_changed();
                }
                break;
            case IdModelChip:
                if (idx >= 0 && idx < static_cast<int>(models_.size())) {
                    sh_.settings.model_name = models_[idx];
                    model_field_.set(to_wide(sh_.settings.model_name));
                    repo_field_.set(L"");
                    refresh_model_files();
                    sh_.save();
                    if (sh_.settings_changed) sh_.settings_changed();
                }
                break;
            case IdModelField: focus_field(model_field_); break;
            case IdRepoField: focus_field(repo_field_); break;
            case IdDownloadModel:
                commit_fields();
                start_model_download();
                break;
            case IdDeviceAuto:
                sh_.settings.device = "auto";
                sh_.save();
                break;
            case IdDeviceCpu:
                sh_.settings.device = "cpu";
                sh_.save();
                break;
            case IdBatchPreset:
                sh_.settings.batch_size = static_cast<uint32_t>(idx);
                batch_field_.set(idx > 0 ? std::to_wstring(idx) : L"");
                sh_.save();
                break;
            case IdBatchField: focus_field(batch_field_); break;
            case IdResetAutoBatch: {
                // Re-read first: the indexer owns the rest of this file and may have written to it since
                IndexState st = load_json_file<IndexState>(state_path());
                st.auto_batch = 0;
                st.auto_batch_key.clear();
                save_json_file(state_path(), st);
                sh_.index_state = std::move(st);
                break;
            }
            case IdBrowseScript:
                if (auto f = pick_path(false, L"Python Script", L"*.py")) {
                    sh_.settings.vision_script = to_utf8(f->wstring());
                    sh_.save();
                }
                break;
            case IdClearScript:
                sh_.settings.vision_script.clear();
                sh_.save();
                break;
            case IdBrowseVenv:
                if (auto dir = pick_path(true)) {
                    sh_.settings.venv_dir = to_utf8(dir->wstring());
                    sh_.save();
                    probe_venv();
                }
                break;
            case IdBackendChip: {
                static constexpr const char* b[4] = {"rocm", "cuda", "cpu", "manual"};
                sh_.settings.torch_backend = b[std::clamp(idx, 0, 3)];
                sh_.save();
                break;
            }
            case IdArchField: focus_field(arch_field_); break;
            case IdCudaPreset:
                sh_.settings.cuda_version = std::string{cuda_versions[static_cast<size_t>(std::clamp(idx, 0, static_cast<int>(cuda_versions.size()) - 1))]};
                cuda_field_.set(to_wide(sh_.settings.cuda_version));
                sh_.save();
                break;
            case IdCudaField: focus_field(cuda_field_); break;
            case IdTorchField: focus_field(torch_field_); break;
            case IdPyVerField: focus_field(py_ver_field_); break;
            case IdPyVerChip: {
                static constexpr const char* v[4] = {"3.14", "3.13", "3.12", "3.11"};
                sh_.settings.python_version = v[std::clamp(idx, 0, 3)];
                py_ver_field_.set(to_wide(sh_.settings.python_version));
                sh_.save();
                break;
            }
            case IdDownloadNugetPy:
                commit_fields();
                run_venv_task([this, in = venv_job_inputs()](const std::stop_token& stop) { download_portable_python(in, stop); });
                return;
            case IdOpenTerminal: open_venv_terminal(); break;
            case IdUpdateVenv:
                run_venv_task([this, in = venv_job_inputs()](const std::stop_token&) { install_packages_in_worker(in); });
                return;
            case IdRecreateVenv:
                if (venv_.portable) {
                    // Portable interpreters are not venvs: re-download the selected version instead
                    run_venv_task([this, in = venv_job_inputs()](const std::stop_token& stop) {
                        std::error_code ec;
                        fs::remove_all(to_wide(in.venv_dir), ec);
                        download_portable_python(in, stop);
                    });
                    return;
                }
                run_venv_task([this, in = venv_job_inputs()](const std::stop_token&) {
                    const fs::path vdir = to_wide(in.venv_dir);
                    std::error_code ec;
                    if (fs::exists(vdir, ec)) {
                        append_venv_log(L"Removing old environment…");
                        fs::remove_all(vdir, ec);
                    }
                    exec_logged(in.base_python, {L"-m", L"venv", vdir.wstring()});
                    install_packages_in_worker(in);
                });
                return;
            case IdDeleteVenv:
                run_venv_task([this, in = venv_job_inputs()](const std::stop_token&) {
                    const fs::path vdir = to_wide(in.venv_dir);
                    std::error_code ec;
                    fs::remove_all(vdir, ec);
                    append_venv_log(L"Virtual environment deleted.");
                });
                return;
            case IdScrollbar:
                if (contains(scroll_thumb_, p)) {
                    dragging_scroll_ = true;
                    drag_y_ = p.y;
                    drag_scroll_start_ = scroll_;
                    SetCapture(hwnd_);
                }
                break;
            default: break;
        }
        invalidate();
    }

    void on_key(UINT vk) {
        if (recording_hotkey_) {
            if (vk == VK_ESCAPE) {
                stop_recording();
                invalidate();
                return;
            }
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return;
            uint32_t mods = 0;
            if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
            if (mods == 0) mods = MOD_ALT;
            sh_.settings.hotkey_mods = mods;
            sh_.settings.hotkey_vk = vk;
            sh_.settings.hotkey_enabled = true;
            sh_.save();
            stop_recording();
            reload_launcher_hotkey();
            invalidate();
            return;
        }
        if (active_field_) {
            if (vk == VK_RETURN) {
                if (active_field_ == &exclude_field_) add_exclude_from_field();
                else commit_fields();
                active_field_->focused = false;
                active_field_ = nullptr;
                invalidate();
                return;
            }
            if (vk == VK_ESCAPE) {
                sync_fields();
                active_field_->focused = false;
                active_field_ = nullptr;
                invalidate();
                return;
            }
            bool handled = false;
            active_field_->on_key(hwnd_, vk, handled);
            active_field_->caret_on = true;
            invalidate();
            return;
        }
        if (vk == VK_ESCAPE) hide();
    }

    void on_char(wchar_t c) {
        if (!active_field_ || recording_hotkey_) return;
        if (active_field_ == &batch_field_ && (c < L'0' || c > L'9')) return;
        if (active_field_->on_char(c)) {
            active_field_->caret_on = true;
            invalidate();
        }
    }

    // ------------------------------------------------------------------------------------------
    // Window procedure
    // ------------------------------------------------------------------------------------------

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            return self->handle(msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        const auto to_dip = [this](LPARAM l) -> D2D1_POINT_2F {
            const float s = target_.scale();
            return {GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s};
        };
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd_, &ps);
                if (visible() || sh_.test_mode) render();
                EndPaint(hwnd_, &ps);
                if (visible() && std::abs(target_scroll_ - scroll_) >= 0.5f) {
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
                mmi->ptMinTrackSize = {static_cast<LONG>(420 * s), static_cast<LONG>(520 * s)};
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd_, &pt);
                    const Hit* h = hit_test(hits_, {pt.x / target_.scale(), pt.y / target_.scale()});
                    const int id = h ? h->id : 0;
                    const bool ibeam = id == IdExcludeField || id == IdModelField || id == IdRepoField || id == IdBatchField || id == IdArchField || id == IdCudaField || id == IdTorchField || id == IdPyVerField;
                    SetCursor(ibeam ? cursor_ibeam_ : (id ? cursor_hand_ : cursor_arrow_));
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: {
                if (!tracking_) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                    TrackMouseEvent(&tme);
                    tracking_ = true;
                }
                flush();
                const D2D1_POINT_2F p = to_dip(lp);
                if (dragging_scroll_) {
                    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                        dragging_scroll_ = false;
                        ReleaseCapture();
                    } else {
                        const float track = view_height() - 8.0f - height(scroll_thumb_);
                        if (track > 0.0f) {
                            scroll_ = target_scroll_ = std::clamp(drag_scroll_start_ + (p.y - drag_y_) / track * max_scroll(), 0.0f, max_scroll());
                        }
                        invalidate();
                        return 0;
                    }
                }
                const Hit* h = hit_test(hits_, p);
                const int id = h ? h->id : IdNone;
                const int idx = h ? h->index : 0;
                if (id != hover_id_ || idx != hover_index_) {
                    hover_id_ = id;
                    hover_index_ = idx;
                    invalidate();
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_ = false;
                if (hover_id_) {
                    hover_id_ = IdNone;
                    invalidate();
                }
                return 0;
            case WM_LBUTTONDOWN: on_lbutton_down(to_dip(lp), false); return 0;
            case WM_LBUTTONDBLCLK: on_lbutton_down(to_dip(lp), true); return 0;
            case WM_LBUTTONUP:
                dragging_scroll_ = false;
                ReleaseCapture();
                return 0;
            case WM_CAPTURECHANGED:
                dragging_scroll_ = false;
                return 0;
            case WM_MOUSEWHEEL:
                target_scroll_ = std::clamp(target_scroll_ - GET_WHEEL_DELTA_WPARAM(wp) * 0.9f, 0.0f, max_scroll());
                start_anim();
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (msg == WM_SYSKEYDOWN && wp == VK_F4 && !recording_hotkey_) break;
                on_key(static_cast<UINT>(wp));
                return 0;
            case WM_CHAR: on_char(static_cast<wchar_t>(wp)); return 0;
            case WM_TIMER:
                switch (wp) {
                    case TIMER_CARET:
                        if (active_field_) {
                            active_field_->caret_on = !active_field_->caret_on;
                            invalidate();
                        }
                        return 0;
                    case TIMER_POLL: {
                        bool changed = sh_.poll_indexer();
                        if (refresh_model_files()) {
                            scan_models(); // a model was added to or removed from the folder
                            changed = true;
                        }
                        if (changed) invalidate();
                        return 0;
                    }
                    case TIMER_DEVICE:
                        KillTimer(hwnd_, TIMER_DEVICE);
                        render();
                        return 0;
                }
                return 0;
            case WM_APP_REDRAW:
                redraw_posted_ = false;
                invalidate();
                return 0;
            case WM_CLOSE:
                hide();
                return 0;
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
    std::atomic<bool> redraw_posted_{false};

    int tab_ = 0;
    AnimFloat tab_anim_;
    AnimFloat sw_hotkey_;
    AnimFloat sw_startup_;
    AnimFloat sw_hide_focus_;
    int hover_id_ = IdNone;
    int hover_index_ = 0;
    bool tracking_ = false;
    bool in_sizemove_ = false;

    float scroll_ = 0.0f, target_scroll_ = 0.0f, content_h_ = 0.0f;
    std::chrono::steady_clock::time_point last_anim_ = std::chrono::steady_clock::now();
    bool dragging_scroll_ = false;
    float drag_y_ = 0.0f, drag_scroll_start_ = 0.0f;
    D2D1_RECT_F scroll_thumb_{};

    TextField exclude_field_;
    std::wstring include_error_;
    std::wstring exclude_error_;
    std::wstring exclude_error_for_text_;
    TextField model_field_;
    TextField repo_field_;
    TextField batch_field_;
    TextField arch_field_;
    TextField cuda_field_;
    TextField torch_field_;
    TextField py_ver_field_;
    TextField* active_field_ = nullptr;
    bool recording_hotkey_ = false;

    struct ModelFiles {
        bool weights = false, tokenizer = false, onnx = false;
        uint64_t weights_bytes = 0;
        bool operator==(const ModelFiles&) const = default;
    };

    std::vector<std::string> models_;
    ModelFiles model_files_;
    VenvInfo venv_;
    std::wstring venv_pkgs_;

    // Last: their threads are joined before anything above is destroyed
    Job venv_job_;
    Job model_job_;
};

} // namespace usyseek::ui
