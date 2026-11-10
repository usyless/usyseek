#pragma once

#include "shared.hpp"
#include "textfield.hpp"

#include <shobjidl.h>

namespace usyseek::ui {

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
        const fs::path sp = vdir / L"Lib" / L"site-packages";
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

    void draw_radio(Canvas& cv, D2D1_RECT_F r, bool on, std::wstring_view title, std::wstring_view desc, int id) {
        const bool hov = hovered(id);
        cv.fill_round(r, 12.0f, on ? with_alpha(color::primary_container, 0.32f) : (hov ? color::surface_high : color::surface_low));
        cv.stroke_round(r, 12.0f, on ? color::primary : color::outline_variant, on ? 1.5f : 1.0f);
        const D2D1_POINT_2F c{r.left + 22.0f, (r.top + r.bottom) * 0.5f};
        cv.stroke_circle(c, 8.0f, on ? color::primary : color::outline, 1.8f);
        if (on) cv.fill_circle(c, 4.5f, color::primary);
        cv.text(title, rect(r.left + 42.0f, r.top + 8.0f, width(r) - 54.0f, 20.0f), Font::BodySemi, color::on_surface);
        cv.text(desc, rect(r.left + 42.0f, r.top + 28.0f, width(r) - 54.0f, 18.0f), Font::Caption, color::on_surface_variant);
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

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        cv.fill(D2D1::RectF(0, 0, sz.width, sz.height), with_alpha(color::bg, 0.88f));

        // Top tab bar
        draw_tabs(cv, sz);

        const D2D1_RECT_F view = D2D1::RectF(0, TABS_H + 1.0f, sz.width, sz.height);
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
        cv.fill(D2D1::RectF(0, 0, sz.width, TABS_H), with_alpha(color::surface_lowest, 0.75f));
        struct TabDef {
            wchar_t g;
            const wchar_t* label;
            int id;
        };
        const TabDef tabs[2] = {
            {glyph::clock, L"Embeddings & Progress", IdTab0},
            {glyph::settings, L"Application & Venv Settings", IdTab1},
        };
        float x = PAD;
        for (int i = 0; i < 2; ++i) {
            const float tw = cv.measure(tabs[i].label, Font::BodySemi);
            const float w = tw + 46.0f;
            const D2D1_RECT_F r = rect(x, 8.0f, w, TABS_H - 16.0f);
            const bool on = tab_ == i;
            if (on) cv.fill_round(r, 10.0f, color::primary_container);
            else if (hovered(tabs[i].id)) cv.fill_round(r, 10.0f, with_alpha(color::on_surface, 0.07f));
            const D2D1_COLOR_F fg = on ? color::on_primary_container : color::on_surface_variant;
            cv.icon(tabs[i].g, rect(x + 12.0f, r.top, 18.0f, height(r)), fg, Font::IconSmall);
            cv.text(tabs[i].label, rect(x + 34.0f, r.top, tw + 4.0f, height(r)), Font::BodySemi, fg);
            cv.hit(r, tabs[i].id, i);
            x += w + 8.0f;
        }
        cv.hline(0, sz.width, TABS_H, with_alpha(color::outline_variant, 0.65f));
    }

    // ------------------------------------------------------------------------------------------
    // Tab 1: Embeddings & Progress
    // ------------------------------------------------------------------------------------------

    float draw_tab_progress(Canvas& cv, float x, float y, float w) {
        // 1. Hero status card
        {
            const float h = 216.0f;
            const float next_y = card_begin(cv, x, y, w, h);
            const float lx = x + 22.0f, rx = x + w - 22.0f;

            cv.text(L"LOCAL MEDIA EMBEDDING STATUS", rect(lx, y + 16.0f, 320.0f, 16.0f), Font::Label, color::primary);

            const bool running = sh_.indexer_running;
            const bool stopping = running && sh_.indexer_stopping;
            const auto& d = sh_.indexer;
            const uint64_t embedded = running ? d.embedded : sh_.db_count.value_or(sh_.index_state.last_index_found);
            const uint64_t total = running && d.found ? d.found : std::max(embedded, sh_.index_state.last_index_found);

            const std::wstring num_a = format_count(embedded);
            const std::wstring num_b = L" / " + format_count(total);
            const float wa = cv.measure(num_a, Font::Hero);
            const float wb = cv.measure(num_b, Font::Title);
            cv.text(num_a, rect(lx, y + 34.0f, wa + 4.0f, 40.0f), Font::Hero, color::on_surface);
            cv.text(num_b, rect(lx + wa + 2.0f, y + 42.0f, wb + 4.0f, 32.0f), Font::Title, color::outline);
            cv.text(L"files embedded", rect(lx + wa + wb + 10.0f, y + 46.0f, 160.0f, 28.0f), Font::Body, color::on_surface_variant);

            // Start / Stop button
            draw_button(cv, rect(rx - 156.0f, y + 26.0f, 156.0f, 40.0f),
                        running ? glyph::pause : glyph::play,
                        stopping ? L"Stopping…" : (running ? L"Stop Indexing" : L"Start Indexing"),
                        IdToggleIndex, 0, running ? 2 : 1);

            // Status badge + rate
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
            const D2D1_RECT_F br = rect(lx, y + 82.0f, bw, 22.0f);
            cv.fill_round(br, 11.0f, with_alpha(badge_col, 0.15f));
            cv.fill_circle({br.left + 11.0f, br.top + 11.0f}, 3.5f, badge_col);
            cv.text(status_txt, rect(br.left + 20.0f, br.top, bw - 22.0f, 22.0f), Font::Caption, badge_col);

            float sx = br.right + 12.0f;
            const uint64_t fail_cnt = running ? d.failed : sh_.failed_files.size();
            if (fail_cnt > 0) {
                const std::wstring ftxt = format_count(fail_cnt) + L" failed";
                const float fw = cv.measure(ftxt, Font::Caption) + 18.0f;
                const D2D1_RECT_F fr = rect(sx, y + 82.0f, fw, 22.0f);
                cv.fill_round(fr, 11.0f, with_alpha(color::error, 0.14f));
                cv.text(ftxt, fr, Font::Caption, color::error, Align::Center);
                sx = fr.right + 12.0f;
            }

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
            cv.text(sub, rect(sx, y + 82.0f, rx - sx, 22.0f), Font::Caption, color::on_surface_variant);

            // Progress bar
            const D2D1_RECT_F track = rect(lx, y + 116.0f, w - 44.0f, 8.0f);
            cv.fill_round(track, 4.0f, color::surface_highest);
            float pct = total > 0 ? static_cast<float>(embedded) / static_cast<float>(total) : (running ? 0.05f : 0.0f);
            pct = std::clamp(pct, 0.0f, 1.0f);
            if (pct > 0.0f) {
                cv.fill_round(rect(track.left, track.top, std::max(8.0f, width(track) * pct), 8.0f), 4.0f, running ? color::amber : color::primary);
            }

            wchar_t pbuf[32];
            swprintf_s(pbuf, L"%.1f%% completed", pct * 100.0f);
            cv.text(pbuf, rect(lx, y + 132.0f, 140.0f, 20.0f), Font::Caption, color::outline);
            std::wstring cur = stopping ? L"Stopping indexer — finishing current batch…"
                             : (running && d.current[0] ? (std::wstring{L"Processing: "} + d.current)
                             : (running && d.message[0] ? std::wstring{d.message} : L"On-demand scanner idle — zero background GPU/RAM usage"));
            cv.text(cur, rect(lx + 150.0f, y + 132.0f, w - 194.0f, 20.0f), Font::MonoSmall, stopping ? color::amber : color::on_surface_variant, Align::Right);

            // Database management row
            const float dby = y + 162.0f;
            cv.hline(lx, rx, dby, with_alpha(color::outline_variant, 0.5f));
            const std::wstring db_info = L"Qdrant collection: " + to_wide(sh_.settings.model_name) +
                                         L"  •  127.0.0.1:" + std::to_wstring(sh_.settings.qdrant_http_port);
            cv.text(db_info, rect(lx, dby + 12.0f, w - 370.0f, 30.0f), Font::Caption, color::outline);
            draw_button(cv, rect(rx - 304.0f, dby + 11.0f, 166.0f, 32.0f), glyph::open, L"Open Qdrant Web UI", IdOpenQdrantUi);
            draw_button(cv, rect(rx - 130.0f, dby + 11.0f, 130.0f, 32.0f), glyph::del, L"Clear Database", IdClearDatabase, 0, 2);
            y = next_y;
        }

        // 1b. Failed files card (shown when decode/embed failures exist)
        if (!sh_.failed_files.empty()) {
            const size_t show_n = std::min<size_t>(sh_.failed_files.size(), 6);
            const float h = 76.0f + show_n * 36.0f + (sh_.failed_files.size() > show_n ? 24.0f : 0.0f);
            const float next_y = card_begin(cv, x, y, w, h);
            const float lx = x + 22.0f, rw = w - 44.0f, rx = lx + rw;

            const std::wstring title = L"Failed Media Files (" + format_count(sh_.failed_files.size()) + L")";
            cv.text(title, rect(lx, y + 16.0f, 320.0f, 22.0f), Font::Large, color::error);
            cv.text(L"Skipped until modified — click a file to reveal in Explorer, or retry all",
                    rect(lx, y + 38.0f, rw - 280.0f, 18.0f), Font::Small, color::on_surface_variant);
            draw_button(cv, rect(rx - 266.0f, y + 18.0f, 126.0f, 32.0f), glyph::open, L"Open Log File", IdOpenFailedLog);
            draw_button(cv, rect(rx - 132.0f, y + 18.0f, 132.0f, 32.0f), glyph::refresh, L"Retry Failed", IdClearFailed);

            float cy = y + 64.0f;
            for (size_t i = 0; i < show_n; ++i) {
                const auto& ff = sh_.failed_files[i];
                const D2D1_RECT_F ir = rect(lx, cy, rw, 32.0f);
                const bool hov = hovered(IdFailedRow, static_cast<int>(i));
                cv.fill_round(ir, 8.0f, hov ? color::surface_high : color::surface_low);
                cv.icon(glyph::warning, rect(ir.left + 8.0f, ir.top, 20.0f, 32.0f), color::error, Font::IconSmall);
                const std::wstring label = ff.path.empty() ? (L"ID " + std::to_wstring(ff.id)) : ff.path;
                cv.text(label, rect(ir.left + 32.0f, ir.top, rw * 0.62f, 32.0f), Font::MonoSmall, color::on_surface);
                cv.text(ff.reason, rect(ir.left + rw * 0.64f, ir.top, rw * 0.36f - 10.0f, 32.0f), Font::Caption, color::outline, Align::Right);
                if (!ff.path.empty()) cv.hit(ir, IdFailedRow, static_cast<int>(i));
                cy += 36.0f;
            }
            if (sh_.failed_files.size() > show_n) {
                const std::wstring more = L"+ " + format_count(sh_.failed_files.size() - show_n) + L" more in " + failed_path(sh_.settings.model_name).filename().wstring();
                cv.text(more, rect(lx, cy, rw, 20.0f), Font::Caption, color::outline);
            }
            y = next_y;
        }

        // 2. Indexing Scope Card
        {
            const bool custom = !sh_.settings.whole_pc;
            const size_t inc_n = sh_.settings.include_dirs.size();
            const float inc_h = custom ? (46.0f + std::max<size_t>(1, inc_n) * 38.0f + 8.0f) : 0.0f;
            const float h = 238.0f + inc_h;
            const float next_y = card_begin(cv, x, y, w, h);
            const float lx = x + 22.0f, rw = w - 44.0f;

            cv.text(L"Indexing Scope", rect(lx, y + 16.0f, 300.0f, 22.0f), Font::Large, color::on_surface);
            cv.text(L"Scan the whole PC with Voidtools Everything, or restrict indexing to specific folders",
                    rect(lx, y + 38.0f, rw, 18.0f), Font::Small, color::on_surface_variant);

            const float half = (rw - 12.0f) * 0.5f;
            draw_radio(cv, rect(lx, y + 66.0f, half, 56.0f), sh_.settings.whole_pc,
                       L"Include the whole PC", L"All local fixed drives (minus exclusions)", IdScopeAll);
            draw_radio(cv, rect(lx + half + 12.0f, y + 66.0f, half, 56.0f), !sh_.settings.whole_pc,
                       L"Specify directories to include", L"Only scan the folders listed below", IdScopeCustom);

            float cy = y + 134.0f;
            if (custom) {
                cv.text(L"INCLUDED FOLDERS", rect(lx, cy + 4.0f, 200.0f, 20.0f), Font::Label, color::primary);
                draw_button(cv, rect(lx + rw - 134.0f, cy, 134.0f, 30.0f), glyph::add, L"Add Directory", IdAddInclude);
                cy += 38.0f;
                if (inc_n == 0) {
                    const D2D1_RECT_F er = rect(lx, cy, rw, 34.0f);
                    cv.fill_round(er, 8.0f, color::surface_low);
                    cv.text(L"No folders added yet — click Add Directory to choose folders to scan", inset(er, 12.0f, 0.0f), Font::Small, color::outline);
                    cy += 38.0f;
                } else {
                    for (size_t i = 0; i < inc_n; ++i) {
                        const D2D1_RECT_F ir = rect(lx, cy, rw, 34.0f);
                        cv.fill_round(ir, 8.0f, color::surface_low);
                        cv.icon(glyph::folder, rect(ir.left + 8.0f, ir.top, 22.0f, 34.0f), color::primary, Font::IconSmall);
                        cv.text(to_wide(sh_.settings.include_dirs[i]), rect(ir.left + 34.0f, ir.top, rw - 72.0f, 34.0f), Font::MonoSmall, color::on_surface);
                        draw_button(cv, rect(ir.right - 32.0f, ir.top + 3.0f, 28.0f, 28.0f), glyph::remove, L"", IdRemoveInclude, static_cast<int>(i), 3);
                        cy += 38.0f;
                    }
                }
                cy += 8.0f;
            }

            // Media types & limits row
            cv.hline(lx, lx + rw, cy, with_alpha(color::outline_variant, 0.5f));
            cy += 12.0f;
            draw_button(cv, rect(lx, cy, 108.0f, 32.0f), glyph::photo, L"Images",
                        IdToggleImages, 0, sh_.settings.index_images ? 1 : 0);
            draw_button(cv, rect(lx + 116.0f, cy, 108.0f, 32.0f), glyph::video, L"Videos",
                        IdToggleVideos, 0, sh_.settings.index_videos ? 1 : 0);

            const std::wstring min_lbl = L"Min size: " + std::to_wstring(sh_.settings.min_file_kb) + L" KB";
            draw_button(cv, rect(lx + rw - 136.0f, cy, 136.0f, 32.0f), 0, min_lbl, IdMinKbStep);

            cy += 42.0f;
            cv.text(L"Click Min size to cycle minimum file size threshold. Videos are embedded by averaging keyframes.",
                    rect(lx, cy, rw, 18.0f), Font::Caption, color::outline);
            y = next_y;
        }

        // 3. Excluded Directories Card (always visible)
        {
            const auto locked = locked_excludes();
            const size_t total_rows = locked.size() + sh_.settings.exclude_dirs.size();
            const float h = 122.0f + total_rows * 36.0f;
            const float next_y = card_begin(cv, x, y, w, h);
            const float lx = x + 22.0f, rw = w - 44.0f;

            cv.text(L"Excluded Directories & Patterns", rect(lx, y + 16.0f, 340.0f, 22.0f), Font::Large, color::on_surface);
            cv.text(L"Skipped in both Whole PC and Custom Folder modes", rect(lx, y + 38.0f, 380.0f, 18.0f), Font::Small, color::on_surface_variant);
            draw_button(cv, rect(lx + rw - 142.0f, y + 18.0f, 142.0f, 32.0f), glyph::folder, L"Add Folder…", IdAddExcludeFolder);

            // Pattern input bar
            float cy = y + 66.0f;
            const D2D1_RECT_F fr = rect(lx, cy, rw - 108.0f, 34.0f);
            cv.fill_round(fr, 8.0f, color::surface_low);
            cv.stroke_round(fr, 8.0f, active_field_ == &exclude_field_ ? color::primary : color::outline_variant);
            exclude_field_.draw(cv, inset(fr, 10.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
            cv.hit(fr, IdExcludeField);
            draw_button(cv, rect(lx + rw - 100.0f, cy, 100.0f, 34.0f), glyph::add, L"Add Rule", IdAddExcludePattern);
            cy += 44.0f;

            for (const auto& sys : locked) {
                const D2D1_RECT_F ir = rect(lx, cy, rw, 32.0f);
                cv.fill_round(ir, 8.0f, with_alpha(color::surface_lowest, 0.7f));
                cv.icon(glyph::block, rect(ir.left + 8.0f, ir.top, 20.0f, 32.0f), color::outline, Font::IconSmall);
                cv.text(sys, rect(ir.left + 32.0f, ir.top, rw - 120.0f, 32.0f), Font::MonoSmall, color::outline);
                cv.text(L"System", rect(ir.right - 92.0f, ir.top, 56.0f, 32.0f), Font::Caption, color::outline, Align::Right);
                cv.icon(glyph::lock, rect(ir.right - 30.0f, ir.top, 24.0f, 32.0f), color::outline, Font::IconSmall);
                cy += 36.0f;
            }
            for (size_t i = 0; i < sh_.settings.exclude_dirs.size(); ++i) {
                const D2D1_RECT_F ir = rect(lx, cy, rw, 32.0f);
                cv.fill_round(ir, 8.0f, color::surface_low);
                const std::wstring wex = to_wide(sh_.settings.exclude_dirs[i]);
                const bool is_pat = wex.find(L'*') != std::wstring::npos || !(wex.size() >= 2 && wex[1] == L':');
                cv.icon(glyph::block, rect(ir.left + 8.0f, ir.top, 20.0f, 32.0f), color::error, Font::IconSmall);
                cv.text(wex, rect(ir.left + 32.0f, ir.top, rw - 120.0f, 32.0f), Font::MonoSmall, color::on_surface);
                cv.text(is_pat ? L"Pattern" : L"Folder", rect(ir.right - 92.0f, ir.top, 56.0f, 32.0f), Font::Caption, color::on_surface_variant, Align::Right);
                draw_button(cv, rect(ir.right - 30.0f, ir.top + 2.0f, 28.0f, 28.0f), glyph::remove, L"", IdRemoveExclude, static_cast<int>(i), 3);
                cy += 36.0f;
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
            const float h = 186.0f;
            const float next_y = card_begin(cv, x, y, w, h);
            const float lx = x + 22.0f, rw = w - 44.0f;

            cv.text(L"Spotlight Invocation Keybind", rect(lx, y + 16.0f, 340.0f, 22.0f), Font::Large, color::on_surface);
            cv.text(L"Global desktop shortcut registered by the lightweight tray launcher", rect(lx, y + 38.0f, rw - 80.0f, 18.0f), Font::Small, color::on_surface_variant);
            draw_switch(cv, lx + rw, y + 30.0f, sh_.settings.hotkey_enabled, IdToggleHotkey);

            // Keybind box
            const D2D1_RECT_F kr = rect(lx, y + 68.0f, 240.0f, 40.0f);
            cv.fill_round(kr, 10.0f, color::surface_lowest);
            cv.stroke_round(kr, 10.0f, recording_hotkey_ ? color::amber : color::outline_variant, recording_hotkey_ ? 1.5f : 1.0f);
            const std::wstring hk = recording_hotkey_ ? L"Press shortcut…" : format_hotkey(sh_.settings.hotkey_mods, sh_.settings.hotkey_vk);
            cv.text(hk, kr, Font::BodySemi, recording_hotkey_ ? color::amber : color::primary, Align::Center);

            draw_button(cv, rect(lx + 252.0f, y + 68.0f, 160.0f, 40.0f), glyph::keyboard,
                        recording_hotkey_ ? L"Cancel Recording" : L"Record Keybind", IdRecordHotkey, 0, recording_hotkey_ ? 1 : 0);
            draw_button(cv, rect(lx + 420.0f, y + 68.0f, 118.0f, 40.0f), 0, L"Reset (Alt+Space)", IdResetHotkey, 0, 3);

            // Pill & startup options row
            const float ry = y + 122.0f;
            cv.hline(lx, lx + rw, ry, with_alpha(color::outline_variant, 0.5f));
            cv.text(L"Open on PC startup", rect(lx, ry + 14.0f, 150.0f, 22.0f), Font::Body, color::on_surface);
            draw_switch(cv, lx + 196.0f, ry + 25.0f, sh_.settings.launch_on_startup, IdToggleStartup);

            cv.text(L"Hide pill when clicking outside", rect(lx + 220.0f, ry + 14.0f, 224.0f, 22.0f), Font::Body, color::on_surface);
            draw_switch(cv, lx + 486.0f, ry + 25.0f, sh_.settings.hide_on_focus_loss, IdToggleHideFocus);

            const std::wstring idle_lbl = sh_.settings.ui_idle_timeout_s < 0
                ? L"UI process timeout: Never"
                : (sh_.settings.ui_idle_timeout_s == 0
                    ? L"UI process timeout: 0s (Immediate)"
                    : L"UI process timeout: " + std::to_wstring(sh_.settings.ui_idle_timeout_s) + L"s");
            draw_button(cv, rect(lx + rw - 238.0f, ry + 10.0f, 238.0f, 32.0f), glyph::clock, idle_lbl, IdIdleStep);
            y = next_y;
        }

        // 2. Embeddings AI Model & GPU Batching card
        {
            const float h = 278.0f;
            const float next_y = card_begin(cv, x, y, w, h);
            const float lx = x + 22.0f, rw = w - 44.0f;

            cv.text(L"Embeddings AI Model & Compute", rect(lx, y + 16.0f, 360.0f, 22.0f), Font::Large, color::on_surface);
            cv.text(L"Vision weights (.safetensors) and text ONNX / tokenizer directory", rect(lx, y + 38.0f, rw, 18.0f), Font::Small, color::on_surface_variant);

            // Model dir row
            float cy = y + 66.0f;
            const D2D1_RECT_F dr = rect(lx, cy, rw - 116.0f, 32.0f);
            cv.fill_round(dr, 8.0f, color::surface_lowest);
            cv.text(to_wide(sh_.settings.model_dir), inset(dr, 10.0f, 0.0f), Font::MonoSmall, color::on_surface_variant);
            draw_button(cv, rect(lx + rw - 108.0f, cy, 108.0f, 32.0f), glyph::folder, L"Browse…", IdBrowseModelDir);

            // Model name chips + custom field
            cy += 42.0f;
            cv.text(L"Model:", rect(lx, cy, 56.0f, 32.0f), Font::SmallSemi, color::on_surface_variant);
            float mx = lx + 58.0f;
            for (size_t i = 0; i < models_.size() && i < 3; ++i) {
                const std::wstring wm = to_wide(models_[i]);
                const float cw_chip = cv.measure(wm, Font::SmallSemi) + 24.0f;
                const bool on = models_[i] == sh_.settings.model_name;
                draw_button(cv, rect(mx, cy, cw_chip, 32.0f), 0, wm, IdModelChip, static_cast<int>(i), on ? 1 : 0);
                mx += cw_chip + 8.0f;
            }
            const D2D1_RECT_F mfr = rect(mx, cy, std::max(140.0f, lx + rw - mx), 32.0f);
            cv.fill_round(mfr, 8.0f, color::surface_low);
            cv.stroke_round(mfr, 8.0f, active_field_ == &model_field_ ? color::primary : color::outline_variant);
            model_field_.draw(cv, inset(mfr, 10.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
            cv.hit(mfr, IdModelField);

            // Device & VRAM Batch size
            cy += 44.0f;
            cv.text(L"Device:", rect(lx, cy, 52.0f, 32.0f), Font::SmallSemi, color::on_surface_variant);
            const bool is_cpu = sh_.settings.device == "cpu";
            draw_button(cv, rect(lx + 54.0f, cy, 138.0f, 32.0f), glyph::chip, L"GPU (ROCm / CUDA)", IdDeviceAuto, 0, !is_cpu ? 1 : 0);
            draw_button(cv, rect(lx + 198.0f, cy, 86.0f, 32.0f), 0, L"CPU Only", IdDeviceCpu, 0, is_cpu ? 1 : 0);

            cv.text(L"Batch size:", rect(lx + 298.0f, cy, 72.0f, 32.0f), Font::SmallSemi, color::on_surface_variant);
            static constexpr uint32_t batches[] = {0, 32, 64, 128, 256};
            float bx = lx + 372.0f;
            for (int i = 0; i < 5; ++i) {
                const std::wstring bl = batches[i] == 0 ? L"Auto (VRAM)" : std::to_wstring(batches[i]);
                const float bw = batches[i] == 0 ? 92.0f : 44.0f;
                draw_button(cv, rect(bx, cy, bw, 32.0f), 0, bl, IdBatchPreset, static_cast<int>(batches[i]),
                            sh_.settings.batch_size == batches[i] ? 1 : 0);
                bx += bw + 5.0f;
            }
            const bool custom_batch = sh_.settings.batch_size > 0 &&
                sh_.settings.batch_size != 32 && sh_.settings.batch_size != 64 &&
                sh_.settings.batch_size != 128 && sh_.settings.batch_size != 256;
            const D2D1_RECT_F bfr = rect(bx, cy, std::max(64.0f, lx + rw - bx), 32.0f);
            cv.fill_round(bfr, 8.0f, color::surface_low);
            cv.stroke_round(bfr, 8.0f, (active_field_ == &batch_field_ || custom_batch) ? color::primary : color::outline_variant,
                            (active_field_ == &batch_field_ || custom_batch) ? 1.5f : 1.0f);
            batch_field_.draw(cv, inset(bfr, 8.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
            cv.hit(bfr, IdBatchField);

            cy += 38.0f;
            if (sh_.settings.batch_size >= 128) {
                cv.icon(glyph::warning, rect(lx, cy, 18.0f, 20.0f), color::amber, Font::IconSmall);
                cv.text(L"High fixed batch sizes may exceed VRAM and trigger driver resets; OOM automatically halves the batch.",
                        rect(lx + 22.0f, cy, rw - 22.0f, 20.0f), Font::Caption, color::amber);
            } else {
                cv.text(L"Auto probes free VRAM on startup and backs off automatically on out-of-memory.",
                        rect(lx, cy, rw, 20.0f), Font::Caption, color::outline);
            }

            // Custom vision script override
            cy += 28.0f;
            cv.text(L"Vision script:", rect(lx, cy, 90.0f, 30.0f), Font::SmallSemi, color::on_surface_variant);
            const std::wstring scr = sh_.settings.vision_script.empty() ? L"Bundled (python\\usyseek_vision.py)" : to_wide(sh_.settings.vision_script);
            const D2D1_RECT_F sr = rect(lx + 92.0f, cy, rw - 260.0f, 30.0f);
            cv.fill_round(sr, 8.0f, color::surface_lowest);
            cv.text(scr, inset(sr, 10.0f, 0.0f), Font::MonoSmall, color::on_surface_variant);
            draw_button(cv, rect(lx + rw - 160.0f, cy, 96.0f, 30.0f), glyph::code, L"Custom…", IdBrowseScript);
            draw_button(cv, rect(lx + rw - 58.0f, cy, 58.0f, 30.0f), 0, L"Reset", IdClearScript, 0, 3);
            y = next_y;
        }

        // 3. Python Virtual Environment (.venv) Card
        {
            const bool has_log = !venv_log_.empty();
            const float h = 330.0f + (has_log ? 84.0f : 0.0f);
            const float next_y = card_begin(cv, x, y, w, h);
            const float lx = x + 22.0f, rw = w - 44.0f;

            cv.text(L"Python Virtual Environment (.venv)", rect(lx, y + 16.0f, 360.0f, 22.0f), Font::Large, color::on_surface);
            cv.text(L"Connected via shared memory (supports any Python 3.10+ .venv with PyTorch & open-clip)",
                    rect(lx, y + 38.0f, rw - 140.0f, 18.0f), Font::Small, color::on_surface_variant);

            // Status pill
            std::error_code ec;
            const bool ver_ok = venv_.exists && fs::exists(venv_.python_exe, ec);
            const std::wstring st_txt = venv_busy_ ? L"Running task…" : (ver_ok ? L".venv Ready" : (venv_.exists ? L"Missing python.exe" : L"Not found"));
            const D2D1_COLOR_F st_col = venv_busy_ ? color::amber : (ver_ok ? color::mint : color::error);
            const float sw = cv.measure(st_txt, Font::Caption) + 26.0f;
            const D2D1_RECT_F sbr = rect(lx + rw - sw, y + 18.0f, sw, 24.0f);
            cv.fill_round(sbr, 12.0f, with_alpha(st_col, 0.16f));
            cv.fill_circle({sbr.left + 11.0f, sbr.top + 12.0f}, 3.5f, st_col);
            cv.text(st_txt, rect(sbr.left + 20.0f, sbr.top, sw - 22.0f, 24.0f), Font::Caption, st_col);

            // Details box
            float cy = y + 66.0f;
            const D2D1_RECT_F box = rect(lx, cy, rw, 84.0f);
            cv.fill_round(box, 10.0f, color::surface_lowest);
            cv.stroke_round(box, 10.0f, color::outline_variant);
            cv.text(L"Venv Path:", rect(lx + 14.0f, cy + 8.0f, 100.0f, 22.0f), Font::Caption, color::outline);
            cv.text(to_wide(sh_.settings.venv_dir), rect(lx + 110.0f, cy + 8.0f, rw - 210.0f, 22.0f), Font::MonoSmall, color::primary);
            draw_button(cv, rect(lx + rw - 92.0f, cy + 6.0f, 84.0f, 24.0f), glyph::folder, L"Select…", IdBrowseVenv);

            cv.text(L"Interpreter:", rect(lx + 14.0f, cy + 32.0f, 100.0f, 22.0f), Font::Caption, color::outline);
            const std::wstring py_info = venv_.exists ? (L"Python " + venv_.version + L"  (" + venv_.home + L")") : L"No pyvenv.cfg in selected folder";
            cv.text(py_info, rect(lx + 110.0f, cy + 32.0f, rw - 120.0f, 22.0f), Font::Small, color::on_surface);

            cv.text(L"Packages:", rect(lx + 14.0f, cy + 56.0f, 100.0f, 22.0f), Font::Caption, color::outline);
            cv.text(venv_pkgs_.empty() ? L"—" : venv_pkgs_, rect(lx + 110.0f, cy + 56.0f, rw - 120.0f, 22.0f), Font::Caption, color::on_surface_variant);

            // Torch installer backend row
            cy += 96.0f;
            cv.text(L"PyTorch stack:", rect(lx, cy, 94.0f, 30.0f), Font::SmallSemi, color::on_surface_variant);
            const char* backends[4] = {"rocm", "cuda", "cpu", "manual"};
            const wchar_t* blabels[4] = {L"AMD ROCm", L"NVIDIA CUDA", L"CPU", L"Manual"};
            float bx = lx + 96.0f;
            for (int i = 0; i < 4; ++i) {
                const bool on = sh_.settings.torch_backend == backends[i];
                const float w_chip = i == 3 ? 78.0f : 98.0f;
                draw_button(cv, rect(bx, cy, w_chip, 30.0f), 0, blabels[i], IdBackendChip, i, on ? 1 : 0);
                bx += w_chip + 6.0f;
            }
            if (sh_.settings.torch_backend == "rocm") {
                cv.text(L"Arch:", rect(bx + 4.0f, cy, 38.0f, 30.0f), Font::Small, color::outline);
                const D2D1_RECT_F ar = rect(bx + 42.0f, cy, 86.0f, 30.0f);
                cv.fill_round(ar, 8.0f, color::surface_low);
                cv.stroke_round(ar, 8.0f, active_field_ == &arch_field_ ? color::primary : color::outline_variant);
                arch_field_.draw(cv, inset(ar, 8.0f, 2.0f), Font::Mono, color::on_surface, color::outline);
                cv.hit(ar, IdArchField);
            }
            draw_button(cv, rect(lx + rw - 196.0f, cy, 196.0f, 30.0f), glyph::terminal, L"Open CMD in .venv", IdOpenTerminal);

            // Optional custom Torch pip args row
            cy += 38.0f;
            cv.text(L"Torch pip args:", rect(lx, cy, 94.0f, 30.0f), Font::SmallSemi, color::on_surface_variant);
            const D2D1_RECT_F tfr = rect(lx + 96.0f, cy, rw - 96.0f, 30.0f);
            cv.fill_round(tfr, 8.0f, color::surface_low);
            cv.stroke_round(tfr, 8.0f, active_field_ == &torch_field_ ? color::primary : color::outline_variant);
            torch_field_.draw(cv, inset(tfr, 10.0f, 2.0f), Font::MonoSmall, color::on_surface, color::outline);
            cv.hit(tfr, IdTorchField);

            // Action buttons
            cy += 42.0f;
            const float bw3 = (rw - 16.0f) / 3.0f;
            draw_button(cv, rect(lx, cy, bw3, 36.0f), glyph::download, L"Install / Update Packages", IdUpdateVenv);
            draw_button(cv, rect(lx + bw3 + 8.0f, cy, bw3, 36.0f), glyph::refresh, L"Create / Recreate Venv", IdRecreateVenv);
            draw_button(cv, rect(lx + (bw3 + 8.0f) * 2.0f, cy, bw3, 36.0f), glyph::del, L"Delete Venv", IdDeleteVenv, 0, 2);

            if (has_log) {
                cy += 46.0f;
                const D2D1_RECT_F lr = rect(lx, cy, rw, 72.0f);
                cv.fill_round(lr, 8.0f, color::surface_lowest);
                cv.text(venv_log_, inset(lr, 10.0f, 6.0f), Font::MonoSmall, color::on_surface_variant);
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
        std::wstring args = fs::exists(act) ? (L"/k \"\"" + act.wstring() + L"\"\"") : L"";
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

    void install_packages_in_worker() {
        const fs::path py = fs::path{to_wide(sh_.settings.venv_dir)} / L"Scripts" / L"python.exe";
        if (!fs::exists(py)) {
            append_venv_log(L"python.exe not found in venv — click Create / Recreate Venv first.");
            return;
        }
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
        return std::max(1.0f, target_.size().height - TABS_H - 1.0f);
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

    void add_exclude_from_field() {
        const std::wstring v = trim(exclude_field_.text);
        if (v.empty()) return;
        sh_.settings.exclude_dirs.push_back(to_utf8(v));
        exclude_field_.set(L"");
        sh_.save();
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
                if (auto dir = pick_path(true)) {
                    sh_.settings.exclude_dirs.push_back(to_utf8(dir->wstring()));
                    sh_.save();
                }
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
            case IdOpenTerminal: open_venv_terminal(); break;
            case IdUpdateVenv:
                run_venv_task([this] { install_packages_in_worker(); });
                return;
            case IdRecreateVenv:
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
                mmi->ptMinTrackSize = {static_cast<LONG>(760 * s), static_cast<LONG>(520 * s)};
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd_, &pt);
                    const Hit* h = hit_test(hits_, {pt.x / target_.scale(), pt.y / target_.scale()});
                    const int id = h ? h->id : 0;
                    const bool ibeam = id == IdExcludeField || id == IdModelField || id == IdBatchField || id == IdArchField || id == IdTorchField;
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
    TextField model_field_;
    TextField batch_field_;
    TextField arch_field_;
    TextField torch_field_;
    TextField* active_field_ = nullptr;
    bool recording_hotkey_ = false;

    std::vector<std::string> models_;
    VenvInfo venv_;
    std::wstring venv_pkgs_;
    std::wstring venv_log_;
    bool venv_busy_ = false;
    std::jthread task_;
};

} // namespace usyseek::ui
