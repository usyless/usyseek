#pragma once

#include "shared.hpp"
#include "flow.hpp"
#include "textfield.hpp"
#include "../exclusions.hpp"

#include <shobjidl.h>
#include <urlmon.h>

namespace usyseek {

struct NugetVersionIndex {
    std::vector<std::string> versions;
};

} // namespace usyseek

namespace usyseek::ui {

class DownloadProgressCallback final : public IBindStatusCallback {
public:
    explicit DownloadProgressCallback(std::function<void(ULONG, ULONG)> on_progress)
        : on_progress_{std::move(on_progress)} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppv = static_cast<IBindStatusCallback*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress, ULONG progress_max, ULONG status_code, LPCWSTR) override {
        if ((status_code == BINDSTATUS_DOWNLOADINGDATA || status_code == BINDSTATUS_ENDDOWNLOADDATA) && on_progress_) {
            on_progress_(progress, progress_max);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return E_NOTIMPL; }

private:
    std::function<void(ULONG, ULONG)> on_progress_;
};

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

    ~SettingsWindow() {
        if (task_.joinable()) task_.join();
    }

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
        torch_field_.placeholder = L"Optional custom pip args (e.g. --index-url ... torch==2.7.1 torchvision==0.22.1)";
        sync_fields();
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
        probe_venv();
        scan_models();
        sh_.poll_indexer(true);
        sh_.refresh_tags();
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
        KillTimer(hwnd_, TIMER_CARET);
        KillTimer(hwnd_, TIMER_POLL);
        KillTimer(hwnd_, TIMER_DEVICE);
        ShowWindow(hwnd_, SW_HIDE);
        ValidateRect(hwnd_, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
    }

    void set_tab(int t) {
        tab_ = t;
        scroll_ = target_scroll_ = 0.0f;
        active_field_ = nullptr;
        render();
    }

    void render() {
        if (rendering_ || (!visible() && !sh_.test_mode)) return;
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
        if (visible() && anim) InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:
    static constexpr float TABS_H = 52.0f;
    static constexpr float PAD = 24.0f;
    static constexpr float CARD_GAP = 16.0f;
    static constexpr UINT_PTR TIMER_CARET = 1, TIMER_POLL = 3, TIMER_DEVICE = 4;

    enum Id {
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
        IdDeviceAuto,
        IdDeviceCpu,
        IdBatchPreset,
        IdBatchField,
        IdBrowseScript,
        IdClearScript,
        IdBrowseVenv,
        IdBrowseBasePy,
        IdPyVerChip,
        IdPyVerField,
        IdDownloadNugetPy,
        IdBackendChip,
        IdArchField,
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
        cv.fill_round(r, 12.0f, on ? color::primary : (hov ? color::surface_highest : color::surface_high));
        if (!on) cv.stroke_round(r, 12.0f, color::outline);
        const float kx = on ? r.right - 12.0f : r.left + 12.0f;
        cv.fill_circle({kx, cy}, on ? 8.0f : 7.0f, on ? color::on_primary : color::outline);
        cv.hit(r, id, idx);
    }

    float text_height(Canvas& cv, std::wstring_view s, Font f, float w, float min_h) {
        return std::max(min_h, cv.measure_height(s, f, w));
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
        for (int i = 0; i < 2; ++i) {
            const FlowPlacement& p = layout.placements[i];
            const float x = PAD + p.x;
            const float tw = cv.measure(tabs[i].label, Font::BodySemi);
            const D2D1_RECT_F r = rect(x, 8.0f + p.y, p.width, TABS_H - 16.0f);
            const bool on = tab_ == i;
            if (on) cv.fill_round(r, 10.0f, color::primary_container);
            else if (hovered(tabs[i].id)) cv.fill_round(r, 10.0f, with_alpha(color::on_surface, 0.07f));
            const D2D1_COLOR_F fg = on ? color::on_primary_container : color::on_surface_variant;
            cv.icon(tabs[i].g, rect(x + 12.0f, r.top, 18.0f, height(r)), fg, Font::IconSmall);
            cv.text(tabs[i].label, rect(x + 34.0f, r.top, tw + 4.0f, height(r)), Font::BodySemi, fg);
            cv.hit(r, tabs[i].id, i);
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
            const uint64_t embedded = running ? d.embedded : sh_.db_count.value_or(sh_.index_state.last_index_found);
            const uint64_t total = running && d.found ? d.found : std::max(embedded, sh_.index_state.last_index_found);

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
            if (custom) {
                cy += include_head.height(rw) + 8.0f;
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
            const auto locked = locked_excludes();
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
            for (const auto& sys : locked) {
                rows.push_back({sys, L"System", true, 0, 0.0f, 0.0f});
            }
            for (size_t i = 0; i < sh_.settings.exclude_dirs.size(); ++i) {
                const std::wstring wex = to_wide(sh_.settings.exclude_dirs[i]);
                const bool is_pat = wex.find(L'*') != std::wstring::npos || !(wex.size() >= 2 && wex[1] == L':');
                rows.push_back({wex, is_pat ? L"Pattern" : L"Folder", false, static_cast<int>(i), 0.0f, 0.0f});
            }

            const float input_y = y + 16.0f + hd.h + 10.0f;
            const float input_h = input.height(rw);
            const float error_y = input_y + input_h + 4.0f;
            const float error_h = show_exclude_error ? text_height(cv, exclude_error_, Font::Caption, rw - 22.0f, 20.0f) : 0.0f;
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
                cv.icon(glyph::warning, rect(lx, error_y, 18.0f, 20.0f), color::error, Font::IconSmall);
                cv.text_wrap(exclude_error_, rect(lx + 22.0f, error_y, rw - 22.0f, error_h), Font::Caption, color::error);
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

    static std::wstring format_hotkey(uint32_t mods, uint32_t vk) {
        std::wstring t;
        if (mods & MOD_CONTROL) t += L"Ctrl + ";
        if (mods & MOD_ALT) t += L"Alt + ";
        if (mods & MOD_SHIFT) t += L"Shift + ";
        if (mods & MOD_WIN) t += L"Win + ";
        wchar_t name[64]{};
        if (vk == VK_SPACE) wcscpy_s(name, L"Space");
        else GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16), name, 64);
        return t + (name[0] ? name : L"Key");
    }

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

            const float keys_y = y + 16.0f + hd.h + 10.0f;
            const float options_line_y = keys_y + keys.height(rw) + 14.0f;
            const float options_y = options_line_y + 10.0f;
            const float thumbs_y = options_y + options.height(rw) + 16.0f;
            const float h = thumbs_y + thumbs.height(rw) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_switch(cv, side.right, side.top + 12.0f, sh_.settings.hotkey_enabled, IdToggleHotkey);
            keys.draw(lx, keys_y, rw);
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
            const float note_x = batch_warn ? 22.0f : 0.0f;
            const float note_h = text_height(cv, batch_note, Font::Caption, rw - note_x, 20.0f);

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
            const float compute_y = chips_y + chips.height(rw) + 12.0f;
            const float note_y = compute_y + compute.height(rw) + 6.0f;
            const float script_y = note_y + note_h + 8.0f;
            const float h = script_y + script.height(rw) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, h);

            draw_header(cv, lx, y, rw, hd);
            dir_row.draw(lx, dir_y, rw);
            chips.draw(lx, chips_y, rw);
            compute.draw(lx, compute_y, rw);
            if (batch_warn) {
                cv.icon(glyph::warning, rect(lx, note_y, 18.0f, 20.0f), color::amber, Font::IconSmall);
            }
            cv.text_wrap(batch_note, rect(lx + note_x, note_y, rw - note_x, note_h), Font::Caption, batch_warn ? color::amber : color::outline);
            script.draw(lx, script_y, rw);
            y = next_y;
        }

        // 3. Python Environment Card (.venv or portable NuGet Python)
        {
            const bool has_log = !venv_log_.empty();
            const float lx = x + 22.0f, rw = w - 44.0f;

            std::error_code ec;
            const bool ver_ok = venv_.exists && fs::exists(venv_.python_exe, ec);
            const std::wstring ready_txt = venv_.portable ? L"Portable Ready" : L".venv Ready";
            const std::wstring st_txt = venv_busy_ ? L"Running task…" : (ver_ok ? ready_txt : (venv_.exists ? L"Missing python.exe" : L"Not found"));
            const D2D1_COLOR_F st_col = venv_busy_ ? color::amber : (ver_ok ? color::mint : color::error);
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
            const std::wstring dl_label = dl_total_ > 0 && venv_busy_
                ? (L"Downloading " + std::to_wstring(dl_done_ * 100 / std::max<ULONG>(1, dl_total_)) + L"%")
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

            const float log_h = has_log ? mono_row_height(cv, venv_log_, rw - 20.0f, 72.0f) : 0.0f;

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
                cv.text_wrap(venv_log_, inset(lr, 10.0f, 6.0f), Font::MonoSmall, color::on_surface_variant);
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

    void run_venv_task(std::function<void()> fn) {
        if (venv_busy_) return;
        if (task_.joinable()) task_.join();
        venv_busy_ = true;
        venv_log_ = L"Starting…";
        render();
        task_ = std::jthread([this, fn = std::move(fn)] {
            fn();
            sh_.call([this] {
                venv_busy_ = false;
                probe_venv();
                render();
            });
        });
    }

    void append_venv_log(std::wstring_view line) {
        const std::wstring clean = trim(line);
        if (clean.empty()) return;
        sh_.call([this, clean] {
            venv_log_ = clean;
            render();
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

    static std::optional<std::string> http_get_to_memory(const wchar_t* url) {
        wil::com_ptr<IStream> stream;
        if (FAILED(URLOpenBlockingStreamW(nullptr, url, &stream, 0, nullptr)) || !stream) return std::nullopt;
        std::string out;
        char buf[16384];
        for (;;) {
            ULONG got = 0;
            const HRESULT hr = stream->Read(buf, sizeof(buf), &got);
            if (got) out.append(buf, got);
            if (FAILED(hr) || hr == S_FALSE || got == 0) break;
        }
        return out;
    }

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
        const auto body = http_get_to_memory(L"https://api.nuget.org/v3-flatcontainer/python/index.json");
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
            if (!v.starts_with(prefix) || v.find('-') != std::string::npos) continue; // stable only
            if (!best || parse_ver(v) > parse_ver(*best)) best = v;
        }
        if (!best) append_venv_log(L"No stable Python " + to_wide(want) + L".x release on NuGet");
        return best;
    }

    void download_portable_python() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto uninit = wil::scope_exit([co] { if (SUCCEEDED(co)) CoUninitialize(); });

        const std::string want = sh_.settings.python_version.empty() ? "3.14" : sh_.settings.python_version;
        const auto ver = resolve_nuget_python(want);
        if (!ver) return;

        const fs::path root = data_dir() / (L"python-" + to_wide(*ver));
        const fs::path pkg = data_dir() / (L"python." + to_wide(*ver) + L".nupkg");
        std::error_code ec;

        if (!fs::exists(root / L"tools" / L"python.exe", ec)) {
            const std::wstring url = L"https://api.nuget.org/v3-flatcontainer/python/" + to_wide(*ver) + L"/python." + to_wide(*ver) + L".nupkg";
            append_venv_log(L"Downloading Python " + to_wide(*ver) + L" from NuGet…");
            DownloadProgressCallback cb{[this](ULONG done, ULONG total) {
                sh_.call([this, done, total] {
                    dl_done_ = done;
                    dl_total_ = total;
                    render();
                });
            }};
            const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), pkg.c_str(), 0, &cb);
            sh_.call([this] { dl_total_ = 0; });
            if (FAILED(hr)) {
                append_venv_log(L"Download failed (HRESULT 0x" + std::format(L"{:08X}", static_cast<uint32_t>(hr)) + L")");
                fs::remove(pkg, ec);
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
            render();
        });
        install_packages_into(py);
    }

    void install_packages_in_worker() {
        const VenvInfo info = read_venv(to_wide(sh_.settings.venv_dir));
        if (!info.exists || !fs::exists(info.python_exe)) {
            append_venv_log(L"python.exe not found — create a venv or download a portable Python first.");
            return;
        }
        install_packages_into(info.python_exe);
    }

    void install_packages_into(const fs::path& py) {
        exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"pip"});
        const std::wstring custom_args = trim(to_wide(sh_.settings.torch_pip_args));
        if (!custom_args.empty()) {
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(custom_args.c_str(), &argc);
            std::vector<std::wstring> args = {L"-m", L"pip", L"install", L"--upgrade"};
            for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
            if (argv) LocalFree(argv);
            exec_logged(py, args);
        } else if (sh_.settings.torch_backend == "manual") {
            append_venv_log(L"Manual PyTorch mode: skipping Torch install, installing requirements.txt…");
        } else if (sh_.settings.torch_backend == "rocm") {
            const std::wstring arch = to_wide(sh_.settings.rocm_arch.empty() ? "gfx1030" : sh_.settings.rocm_arch);
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"--index-url", L"https://stable.repo.amd.com/rocm/whl-next/",
                             L"torch[device-" + arch + L"]", L"torchvision[device-" + arch + L"]", L"rocm[libraries,device-" + arch + L"]"});
        } else if (sh_.settings.torch_backend == "cuda") {
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"--index-url", L"https://download.pytorch.org/whl/cu126",
                             L"torch", L"torchvision"});
        } else {
            exec_logged(py, {L"-m", L"pip", L"install", L"--upgrade", L"--index-url", L"https://download.pytorch.org/whl/cpu",
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

    void commit_fields() {
        bool changed = false;
        const std::string m = to_utf8(trim(model_field_.text));
        if (!m.empty() && m != sh_.settings.model_name) {
            sh_.settings.model_name = m;
            changed = true;
        }
        const std::string a = to_utf8(trim(arch_field_.text));
        if (!a.empty() && a != sh_.settings.rocm_arch) {
            sh_.settings.rocm_arch = a;
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
        if (is_duplicate_exclusion(v, existing)) {
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
        render();
    }

    void stop_recording() {
        if (!recording_hotkey_) return;
        recording_hotkey_ = false;
        send_cmd_value(find_launcher(), Cmd::SuspendHotkey, uint32_t{0});
    }

    void on_lbutton_down(D2D1_POINT_2F p, bool dbl) {
        SetFocus(hwnd_);
        const Hit* h = hit_test(hits_, p);
        const int id = h ? h->id : IdNone;
        const int idx = h ? h->index : 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;

        if (active_field_ && id != IdExcludeField && id != IdModelField && id != IdBatchField && id != IdArchField && id != IdTorchField) {
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
                    sh_.settings.include_dirs.push_back(to_utf8(dir->wstring()));
                    sh_.save();
                }
                break;
            case IdRemoveInclude:
                if (idx >= 0 && idx < static_cast<int>(sh_.settings.include_dirs.size())) {
                    sh_.settings.include_dirs.erase(sh_.settings.include_dirs.begin() + idx);
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
                send_to_launcher(Cmd::ReloadSettings);
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
                send_to_launcher(Cmd::ReloadSettings);
                break;
            case IdToggleStartup:
                sh_.settings.launch_on_startup = !sh_.settings.launch_on_startup;
                set_startup_enabled(sh_.settings.launch_on_startup);
                sh_.save();
                break;
            case IdToggleHideFocus:
                sh_.settings.hide_on_focus_loss = !sh_.settings.hide_on_focus_loss;
                sh_.save();
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
                    sh_.save();
                    if (sh_.settings_changed) sh_.settings_changed();
                }
                break;
            case IdModelField: focus_field(model_field_); break;
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
                run_venv_task([this] { download_portable_python(); });
                return;
            case IdOpenTerminal: open_venv_terminal(); break;
            case IdUpdateVenv:
                run_venv_task([this] { install_packages_in_worker(); });
                return;
            case IdRecreateVenv:
                if (venv_.portable) {
                    // Portable interpreters are not venvs: re-download the selected version instead
                    run_venv_task([this] {
                        std::error_code ec;
                        fs::remove_all(to_wide(sh_.settings.venv_dir), ec);
                        download_portable_python();
                    });
                    return;
                }
                run_venv_task([this] {
                    const fs::path base = find_base_python();
                    const fs::path vdir = to_wide(sh_.settings.venv_dir);
                    std::error_code ec;
                    if (fs::exists(vdir, ec)) {
                        append_venv_log(L"Removing old environment…");
                        fs::remove_all(vdir, ec);
                    }
                    exec_logged(base, {L"-m", L"venv", vdir.wstring()});
                    install_packages_in_worker();
                });
                return;
            case IdDeleteVenv:
                run_venv_task([this] {
                    const fs::path vdir = to_wide(sh_.settings.venv_dir);
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
        render();
    }

    void on_key(UINT vk) {
        if (recording_hotkey_) {
            if (vk == VK_ESCAPE) {
                stop_recording();
                render();
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
            send_to_launcher(Cmd::ReloadSettings);
            render();
            return;
        }
        if (active_field_) {
            if (vk == VK_RETURN) {
                if (active_field_ == &exclude_field_) add_exclude_from_field();
                else commit_fields();
                active_field_->focused = false;
                active_field_ = nullptr;
                render();
                return;
            }
            if (vk == VK_ESCAPE) {
                sync_fields();
                active_field_->focused = false;
                active_field_ = nullptr;
                render();
                return;
            }
            bool handled = false;
            active_field_->on_key(hwnd_, vk, handled);
            active_field_->caret_on = true;
            render();
            return;
        }
        if (vk == VK_ESCAPE) hide();
    }

    void on_char(wchar_t c) {
        if (!active_field_ || recording_hotkey_) return;
        if (active_field_ == &batch_field_ && (c < L'0' || c > L'9')) return;
        if (active_field_->on_char(c)) {
            active_field_->caret_on = true;
            render();
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
                    const bool ibeam = id == IdExcludeField || id == IdModelField || id == IdBatchField || id == IdArchField || id == IdTorchField || id == IdPyVerField;
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
                        render();
                        return 0;
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
                return 0;
            }
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
                            render();
                        }
                        return 0;
                    case TIMER_POLL:
                        if (sh_.poll_indexer()) render();
                        return 0;
                    case TIMER_DEVICE:
                        KillTimer(hwnd_, TIMER_DEVICE);
                        render();
                        return 0;
                }
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

    int tab_ = 0;
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
    std::wstring exclude_error_;
    std::wstring exclude_error_for_text_;
    TextField model_field_;
    TextField batch_field_;
    TextField arch_field_;
    TextField torch_field_;
    TextField py_ver_field_;
    TextField* active_field_ = nullptr;
    bool recording_hotkey_ = false;

    std::vector<std::string> models_;
    VenvInfo venv_;
    std::wstring venv_pkgs_;
    std::wstring venv_log_;
    bool venv_busy_ = false;
    ULONG dl_done_ = 0, dl_total_ = 0;
    std::jthread task_;
};

} // namespace usyseek::ui
