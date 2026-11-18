#pragma once

#include "shared.hpp"
#include "textfield.hpp"
#include "widgets.hpp"

#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace usyseek::ui {

/// Dedicated window for creating, duplicating, and editing custom JSON themes.
class ThemeEditorWindow {
public:
    explicit ThemeEditorWindow(Shared& s) : sh_{s} {}

    bool create(HINSTANCE inst) {
        inst_ = inst;
        WNDCLASSEXW wc{.cbSize = sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &ThemeEditorWindow::wndproc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = THEME_EDITOR_CLASS;
        RegisterClassExW(&wc);

        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        const UINT sys_dpi = GetDpiForSystem();
        const float s = (sys_dpi > 0 ? static_cast<float>(sys_dpi) : 96.0f) / 96.0f;
        RECT rc{.left = 0, .top = 0, .right = static_cast<LONG>(740.0f * s), .bottom = static_cast<LONG>(760.0f * s)};
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, sys_dpi);

        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, THEME_EDITOR_CLASS, L"Theme Editor \u2014 usyseek", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, inst, this);
        if (!hwnd_) return false;

        cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
        cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);

        icons_.apply(hwnd_, inst_, 101);
        target_.attach(sh_.gfx, hwnd_, glass_active());
        apply_window_theme(hwnd_, sh_.theme, false, true);
        name_field_.placeholder = L"Custom Theme";
        load_draft_into_fields();
        return true;
    }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool visible() const noexcept { return IsWindowVisible(hwnd_) != FALSE; }

    void open_new() {
        clear_unsaved_preview();
        draft_ = ThemeSpec{};
        draft_.name = "Custom Theme";
        draft_.base = "dark";
        editing_existing_ = false;
        target_path_.clear();
        reset_open_state();
        show_window();
    }

    void open_duplicate(const ThemeEntry& src) {
        clear_unsaved_preview();
        const std::string base_name = src.name.empty() ? "Custom Theme" : src.name;
        draft_ = make_duplicate_theme_spec(src, ulp::str::concat_strings(base_name, " Copy"));
        editing_existing_ = false;
        target_path_.clear();
        reset_open_state();
        show_window();
    }

    void open_edit(const ThemeEntry& src) {
        if (!src.custom || is_example_theme_entry(src) || !src.spec || src.path.empty()) {
            open_duplicate(src);
            return;
        }
        clear_unsaved_preview();
        draft_ = *src.spec;
        if (draft_.name.empty()) draft_.name = src.name;
        editing_existing_ = true;
        target_path_ = src.path;
        reset_open_state();
        show_window();
    }

    void apply_theme() noexcept {
        if (!hwnd_) return;
        apply_window_theme(hwnd_, sh_.theme, false, true);
        target_.set_liquid_glass(glass_active());
        if (glass_active()) {
            if (visible() && !sh_.test_mode) SetTimer(hwnd_, TIMER_GLASS, 16, nullptr);
        } else {
            KillTimer(hwnd_, TIMER_GLASS);
            glass_.release();
            sh_.desktop_dupl.reset();
        }
        full_dirty_ = true;
        invalidate();
    }

    void hide(bool revert_preview = true) {
        if (revert_preview) clear_unsaved_preview();
        if (active_field_) {
            active_field_->focused = false;
            active_field_ = nullptr;
        }
        if (active_slider_ >= 0) {
            active_slider_ = -1;
            ReleaseCapture();
        }
        dragging_scroll_ = false;
        hover_.set(IdNone, 0, false);
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        anim_active_ = false;
        full_dirty_ = true;
        KillTimer(hwnd_, TIMER_CARET);
        KillTimer(hwnd_, TIMER_GLASS);
        ShowWindow(hwnd_, SW_HIDE);
        ValidateRect(hwnd_, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
    }

    void invalidate() noexcept {
        full_dirty_ = true;
        if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:
    static constexpr float HEADER_H = 62.0f;
    static constexpr float FOOTER_H = 58.0f;
    static constexpr float PAD = 20.0f;
    static constexpr float CARD_GAP = 14.0f;
    static constexpr UINT_PTR TIMER_CARET = 1, TIMER_GLASS = 2;

    enum Id : uint8_t {
        IdNone = 0,
        IdNameField,
        IdBaseChip,
        IdModeChip,
        IdToggleLiquidGlass,
        IdToggleRoundCorners,
        IdPillBackdropChip,
        IdSettingsBackdropChip,
        IdSlider,
        IdResetSlider,
        IdTogglePillRadius,
        IdResetGlass,
        IdColorSwatch,
        IdColorField,
        IdResetColor,
        IdToggleAdvancedColors,
        IdTagSwatch,
        IdTagField,
        IdResetTag,
        IdSaveApply,
        IdApplyOnly,
        IdDeleteTheme,
        IdCancel,
        IdScrollbar,
    };

    enum SliderIdx : uint8_t {
        SlPillAlpha = 0,
        SlSettingsAlpha,
        SlCardAlpha,
        SlShadowScale,
        SlPillRadius,
        SlCardRadius,
        SlButtonRadius,
        SlThumbRadius,
        SlGlassBlur,
        SlGlassRoundness,
        SlGlassRefraction,
        SlGlassDispersion,
        SlGlassGlareScale,
        SlGlassGlareAngle,
        SlCount,
    };

    struct ColorFieldDef {
        std::string_view key;
        std::wstring_view label;
        std::wstring_view hint;
        std::string ThemeSpec::* spec_member;
        D2D1_COLOR_F Theme::* theme_member;
        bool advanced;
    };

    static constexpr std::array<ColorFieldDef, 23> kColorDefs{{
        {.key = "primary", .label = L"Primary Accent", .hint = L"Highlights, primary buttons, and active selection", .spec_member = &ThemeSpec::primary, .theme_member = &Theme::primary, .advanced = false},
        {.key = "primary_hover", .label = L"Primary Hover", .hint = L"Hover state of primary buttons and accents", .spec_member = &ThemeSpec::primary_hover, .theme_member = &Theme::primary_hover, .advanced = false},
        {.key = "on_primary", .label = L"Text on Primary", .hint = L"Foreground text & icons over primary accent", .spec_member = &ThemeSpec::on_primary, .theme_member = &Theme::on_primary, .advanced = false},
        {.key = "bg", .label = L"Window Background", .hint = L"Main backdrop tint under panels", .spec_member = &ThemeSpec::bg, .theme_member = &Theme::bg, .advanced = false},
        {.key = "surface", .label = L"Card Surface", .hint = L"Cards and result tiles background", .spec_member = &ThemeSpec::surface, .theme_member = &Theme::surface, .advanced = false},
        {.key = "surface_high", .label = L"Elevated Surface", .hint = L"Secondary buttons, switches, and chips", .spec_member = &ThemeSpec::surface_high, .theme_member = &Theme::surface_high, .advanced = false},
        {.key = "on_surface", .label = L"Primary Text", .hint = L"Main titles and body copy", .spec_member = &ThemeSpec::on_surface, .theme_member = &Theme::on_surface, .advanced = false},
        {.key = "on_surface_variant", .label = L"Secondary Text", .hint = L"Captions, paths, and subtitles", .spec_member = &ThemeSpec::on_surface_variant, .theme_member = &Theme::on_surface_variant, .advanced = false},
        {.key = "outline", .label = L"Outline / Muted", .hint = L"Inactive icons and key borders", .spec_member = &ThemeSpec::outline, .theme_member = &Theme::outline, .advanced = false},
        {.key = "pill_border", .label = L"Pill Border", .hint = L"Outer border color of the search pill", .spec_member = &ThemeSpec::pill_border, .theme_member = &Theme::pill_border, .advanced = false},

        {.key = "surface_lowest", .label = L"Lowest Surface", .hint = L"Recessed wells and text input fields", .spec_member = &ThemeSpec::surface_lowest, .theme_member = &Theme::surface_lowest, .advanced = true},
        {.key = "surface_low", .label = L"Low Surface", .hint = L"Nested panels and secondary containers", .spec_member = &ThemeSpec::surface_low, .theme_member = &Theme::surface_low, .advanced = true},
        {.key = "surface_highest", .label = L"Highest Surface", .hint = L"Hovered buttons and slider tracks", .spec_member = &ThemeSpec::surface_highest, .theme_member = &Theme::surface_highest, .advanced = true},
        {.key = "outline_variant", .label = L"Subtle Outline", .hint = L"Card strokes and subtle hairlines", .spec_member = &ThemeSpec::outline_variant, .theme_member = &Theme::outline_variant, .advanced = true},
        {.key = "primary_container", .label = L"Primary Container", .hint = L"Soft accent fill for selected chips", .spec_member = &ThemeSpec::primary_container, .theme_member = &Theme::primary_container, .advanced = true},
        {.key = "on_primary_container", .label = L"On Primary Container", .hint = L"Text inside soft accent containers", .spec_member = &ThemeSpec::on_primary_container, .theme_member = &Theme::on_primary_container, .advanced = true},
        {.key = "secondary_container", .label = L"Secondary Container", .hint = L"Subtle pill badges and secondary tags", .spec_member = &ThemeSpec::secondary_container, .theme_member = &Theme::secondary_container, .advanced = true},
        {.key = "on_secondary_container", .label = L"On Secondary Container", .hint = L"Text inside secondary containers", .spec_member = &ThemeSpec::on_secondary_container, .theme_member = &Theme::on_secondary_container, .advanced = true},
        {.key = "error", .label = L"Error Accent", .hint = L"Error indicators and destructive buttons", .spec_member = &ThemeSpec::error, .theme_member = &Theme::error, .advanced = true},
        {.key = "error_container", .label = L"Error Container", .hint = L"Soft fill for error banners", .spec_member = &ThemeSpec::error_container, .theme_member = &Theme::error_container, .advanced = true},
        {.key = "mint", .label = L"Success / Mint", .hint = L"Completion and healthy status indicators", .spec_member = &ThemeSpec::mint, .theme_member = &Theme::mint, .advanced = true},
        {.key = "amber", .label = L"Warning / Amber", .hint = L"Warning badges and caution highlights", .spec_member = &ThemeSpec::amber, .theme_member = &Theme::amber, .advanced = true},
        {.key = "scrim", .label = L"Overlay Scrim", .hint = L"Media preview darkroom backdrop", .spec_member = &ThemeSpec::scrim, .theme_member = &Theme::scrim, .advanced = true},
    },};

    struct NamedOption {
        std::string_view value;
        std::wstring_view label;
    };

    static constexpr std::array<NamedOption, 8> kBaseOptions{{
        {.value = "dark", .label = L"Dark"},
        {.value = "light", .label = L"Light"},
        {.value = "system", .label = L"System"},
        {.value = "accent", .label = L"Accent"},
        {.value = "nord", .label = L"Nord"},
        {.value = "mocha", .label = L"Mocha"},
        {.value = "oled", .label = L"OLED"},
        {.value = "liquid-glass", .label = L"Liquid Glass"},
    },};

    static constexpr std::array<NamedOption, 4> kModeOptions{{
        {.value = "", .label = L"Default"},
        {.value = "dark", .label = L"Dark"},
        {.value = "light", .label = L"Light"},
        {.value = "system", .label = L"Windows"},
    },};

    static constexpr std::array<NamedOption, 5> kBackdropOptions{{
        {.value = "", .label = L"Default"},
        {.value = "acrylic", .label = L"Acrylic"},
        {.value = "mica", .label = L"Mica"},
        {.value = "tabbed", .label = L"Tabbed"},
        {.value = "none", .label = L"Solid"},
    },};

    [[nodiscard]] bool glass_active() const noexcept { return sh_.theme.is_liquid_glass; }

    void show_window() {
        sync_switch_targets(false);
        if (!visible()) {
            ShowWindow(hwnd_, SW_SHOWNORMAL);
        }
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        if (glass_active()) {
            ensure_window_capture_affinity(hwnd_, true);
            if (!sh_.test_mode) SetTimer(hwnd_, TIMER_GLASS, 16, nullptr);
        }
        full_dirty_ = true;
        render();
        ValidateRect(hwnd_, nullptr);
        if (sh_.visibility_changed) sh_.visibility_changed();
    }

    void reset_open_state() {
        scroll_ = target_scroll_ = 0.0f;
        show_advanced_colors_ = false;
        preview_applied_ = false;
        error_msg_.clear();
        if (active_field_) {
            active_field_->focused = false;
            active_field_ = nullptr;
        }
        mark_preview_dirty();
        load_draft_into_fields();
        sync_switch_targets(false);
    }

    void mark_preview_dirty() noexcept { preview_dirty_ = true; }

    const Theme& resolved_preview() {
        if (preview_dirty_) {
            const std::string fallback = draft_.name.empty() ? "Custom Theme" : draft_.name;
            preview_ = apply_theme_spec(draft_, "preview", fallback);
            for (size_t i = 0; i < kColorDefs.size(); ++i) {
                const D2D1_COLOR_F c = preview_.*kColorDefs[i].theme_member;
                color_placeholders_[i] = to_wide(format_hex_color(c, c.a < 0.999f));
            }
            for (size_t i = 0; i < preview_.tag_palette.size(); ++i) {
                const D2D1_COLOR_F c = preview_.tag_palette[i];
                tag_placeholders_[i] = to_wide(format_hex_color(c, c.a < 0.999f));
            }
            preview_dirty_ = false;
        }
        return preview_;
    }

    void load_draft_into_fields() {
        name_field_.set(to_wide(draft_.name));
        for (size_t i = 0; i < kColorDefs.size(); ++i) {
            color_fields_[i].set(to_wide(draft_.*kColorDefs[i].spec_member));
        }
        for (size_t i = 0; i < tag_fields_.size(); ++i) {
            const std::string s = i < draft_.tag_palette.size() ? draft_.tag_palette[i] : std::string{};
            tag_fields_[i].set(to_wide(s));
        }
        mark_preview_dirty();
        (void)resolved_preview();
    }

    void commit_field(TextField* tf) {
        if (!tf) return;
        if (tf == &name_field_) {
            std::string trimmed = to_utf8(name_field_.text);
            while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
            draft_.name = trimmed.empty() ? "Custom Theme" : trimmed;
            name_field_.set(to_wide(draft_.name));
            mark_preview_dirty();
            return;
        }
        for (size_t i = 0; i < color_fields_.size(); ++i) {
            if (tf == &color_fields_[i]) {
                apply_hex_field_value(i, false);
                return;
            }
        }
        for (size_t i = 0; i < tag_fields_.size(); ++i) {
            if (tf == &tag_fields_[i]) {
                apply_tag_field_value(i, false);
                return;
            }
        }
    }

    void apply_hex_field_value(size_t i, bool live_typing) {
        std::string val = to_utf8(color_fields_[i].text);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        std::string& slot = draft_.*kColorDefs[i].spec_member;
        if (val.empty()) {
            slot.clear();
            mark_preview_dirty();
            return;
        }
        if (const auto parsed = parse_hex_color(val)) {
            const std::string norm = format_hex_color(*parsed, parsed->a < 0.999f);
            slot = norm;
            if (!live_typing) color_fields_[i].set(to_wide(norm));
            mark_preview_dirty();
        } else if (!live_typing) {
            color_fields_[i].set(to_wide(slot));
        }
    }

    void apply_tag_field_value(size_t i, bool live_typing) {
        std::string val = to_utf8(tag_fields_[i].text);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        if (val.empty()) {
            if (i < draft_.tag_palette.size()) {
                draft_.tag_palette[i].clear();
                trim_empty_tag_palette();
                mark_preview_dirty();
            }
            return;
        }
        if (const auto parsed = parse_hex_color(val)) {
            if (draft_.tag_palette.size() <= i) draft_.tag_palette.resize(i + 1);
            const std::string norm = format_hex_color(*parsed, parsed->a < 0.999f);
            draft_.tag_palette[i] = norm;
            if (!live_typing) tag_fields_[i].set(to_wide(norm));
            mark_preview_dirty();
        } else if (!live_typing) {
            const std::string cur = i < draft_.tag_palette.size() ? draft_.tag_palette[i] : std::string{};
            tag_fields_[i].set(to_wide(cur));
        }
    }

    void trim_empty_tag_palette() {
        const bool any = std::ranges::any_of(draft_.tag_palette, [](const std::string& s) { return !s.empty(); });
        if (!any) draft_.tag_palette.clear();
    }

    void pick_color_native(size_t idx, bool is_tag) {
        if (in_modal_) return;
        commit_field(active_field_);
        const Theme& t = resolved_preview();
        const D2D1_COLOR_F cur = is_tag ? t.tag_palette[idx] : t.*kColorDefs[idx].theme_member;
        const auto byte = [](float v) -> BYTE {
            return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        static std::array<COLORREF, 16> cust_colors{{
            RGB(122, 162, 247), RGB(125, 211, 252), RGB(158, 206, 106), RGB(224, 175, 104),
            RGB(247, 118, 142), RGB(187, 154, 247), RGB(244, 143, 177), RGB(136, 192, 208),
            RGB(22, 22, 30),    RGB(30, 30, 46),    RGB(246, 248, 252), RGB(255, 255, 255),
            RGB(15, 23, 42),    RGB(56, 189, 248),  RGB(166, 227, 161), RGB(243, 139, 168),
        },};

        CHOOSECOLORW cc{.lStructSize = sizeof(cc)};
        cc.hwndOwner = hwnd_;
        cc.rgbResult = RGB(byte(cur.r), byte(cur.g), byte(cur.b));
        cc.lpCustColors = cust_colors.data();
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;

        in_modal_ = true;
        const BOOL ok = ChooseColorW(&cc);
        in_modal_ = false;
        if (!ok) return;

        const D2D1_COLOR_F picked{
            .r = static_cast<float>(GetRValue(cc.rgbResult)) / 255.0f,
            .g = static_cast<float>(GetGValue(cc.rgbResult)) / 255.0f,
            .b = static_cast<float>(GetBValue(cc.rgbResult)) / 255.0f,
            .a = cur.a,
        };
        const std::string hex_s = format_hex_color(picked, picked.a < 0.999f);
        if (is_tag) {
            if (draft_.tag_palette.size() <= idx) draft_.tag_palette.resize(idx + 1);
            draft_.tag_palette[idx] = hex_s;
            tag_fields_[idx].set(to_wide(hex_s));
        } else {
            draft_.*kColorDefs[idx].spec_member = hex_s;
            color_fields_[idx].set(to_wide(hex_s));
        }
        mark_preview_dirty();
        invalidate();
    }

    void sync_switch_targets(bool animate) {
        const Theme& t = resolved_preview();
        sw_glass_.set(t.is_liquid_glass ? 1.0f : 0.0f, animate);
        sw_round_.set(t.round_corners ? 1.0f : 0.0f, animate);
        sw_pill_rad_.set(draft_.pill_radius.has_value() ? 1.0f : 0.0f, animate);
        sw_adv_.set(show_advanced_colors_ ? 1.0f : 0.0f, animate);
        if (animate && !sh_.test_mode) start_anim();
    }

    void clear_unsaved_preview() {
        if (!preview_applied_) return;
        preview_applied_ = false;
        sh_.theme_spec_override.reset();
        sh_.refresh_theme(false, false);
    }

    void apply_preview_only() {
        commit_field(active_field_);
        preview_applied_ = true;
        sh_.theme_spec_override = draft_;
        sh_.refresh_theme(false, false);
    }

    void save_and_apply() {
        commit_field(active_field_);
        error_msg_.clear();
        const std::string clean_name = draft_.name.empty() ? "Custom Theme" : draft_.name;
        draft_.name = clean_name;

        const fs::path dest = (editing_existing_ && !target_path_.empty())
            ? target_path_
            : unique_theme_path(clean_name);

        if (!save_theme_file(dest, draft_)) {
            error_msg_ = ulp::str::concat_strings(L"Failed to write theme file: ", dest.filename().wstring());
            sh_.notify("error", "Themes", "Could not save custom theme", to_utf8(error_msg_), "theme-save");
            invalidate();
            return;
        }

        sh_.clear_notification_key("Themes", "theme-save");
        preview_applied_ = false;
        sh_.theme_spec_override.reset();
        sh_.theme_override.reset();
        sh_.settings.theme = ulp::str::concat_strings("custom:", to_utf8(dest.stem().wstring()));
        sh_.refresh_theme(true, true);
        sh_.save();
        hide(false);
    }

    void delete_current_theme() {
        if (!editing_existing_ || target_path_.empty() || in_modal_) return;
        const std::wstring msg = ulp::str::concat_strings(
            L"Delete custom theme \"", to_wide(draft_.name), L"\"?\n\nFile: ", target_path_.wstring());
        in_modal_ = true;
        const bool confirmed = confirm_box(hwnd_, L"Delete Custom Theme", msg);
        in_modal_ = false;
        if (!confirmed) return;

        std::error_code ec;
        fs::remove(target_path_, ec);
        if (ec) {
            error_msg_ = ulp::str::concat_strings(L"Failed to delete theme file: ", to_wide(ec.message()));
            invalidate();
            return;
        }

        const std::string deleted_id = ulp::str::concat_strings("custom:", to_utf8(target_path_.stem().wstring()));
        preview_applied_ = false;
        sh_.theme_spec_override.reset();
        if (sh_.settings.theme == deleted_id) {
            sh_.settings.theme = "system";
            sh_.save();
        }
        sh_.refresh_theme(true, true);
        hide(false);
    }

    // ------------------------------------------------------------------------------------------
    // Slider helpers
    // ------------------------------------------------------------------------------------------

    struct SliderSpec {
        float min_v;
        float max_v;
        float step;
        std::wstring_view label;
        const wchar_t* unit;
        int decimals;
    };

    static constexpr SliderSpec slider_spec(int idx) noexcept {
        switch (idx) {
            case SlPillAlpha: return {.min_v = 0.05f, .max_v = 1.0f, .step = 0.01f, .label = L"Search Pill Opacity", .unit = L"", .decimals = 2};
            case SlSettingsAlpha: return {.min_v = 0.05f, .max_v = 1.0f, .step = 0.01f, .label = L"Settings & Editor Opacity", .unit = L"", .decimals = 2};
            case SlCardAlpha: return {.min_v = 0.05f, .max_v = 1.0f, .step = 0.01f, .label = L"Card Background Opacity", .unit = L"", .decimals = 2};
            case SlShadowScale: return {.min_v = 0.0f, .max_v = 2.0f, .step = 0.05f, .label = L"Shadow Intensity", .unit = L"\u00d7", .decimals = 2};
            case SlPillRadius: return {.min_v = 0.0f, .max_v = 48.0f, .step = 1.0f, .label = L"Pill Corner Radius", .unit = L" px", .decimals = 0};
            case SlCardRadius: return {.min_v = 0.0f, .max_v = 32.0f, .step = 1.0f, .label = L"Card Corner Radius", .unit = L" px", .decimals = 0};
            case SlButtonRadius: return {.min_v = 0.0f, .max_v = 24.0f, .step = 1.0f, .label = L"Button & Input Radius", .unit = L" px", .decimals = 0};
            case SlThumbRadius: return {.min_v = 0.0f, .max_v = 24.0f, .step = 1.0f, .label = L"Thumbnail Radius", .unit = L" px", .decimals = 0};
            case SlGlassBlur: return {.min_v = 0.0f, .max_v = 6.0f, .step = 0.1f, .label = L"Desktop Blur Sigma", .unit = L"", .decimals = 1};
            case SlGlassRoundness: return {.min_v = 2.0f, .max_v = 8.0f, .step = 0.1f, .label = L"Superellipse Exponent", .unit = L"", .decimals = 1};
            case SlGlassRefraction: return {.min_v = 1.0f, .max_v = 2.0f, .step = 0.02f, .label = L"Refraction Index (IOR)", .unit = L"", .decimals = 2};
            case SlGlassDispersion: return {.min_v = 0.0f, .max_v = 12.0f, .step = 0.2f, .label = L"Chromatic Dispersion", .unit = L"", .decimals = 1};
            case SlGlassGlareScale: return {.min_v = 0.0f, .max_v = 2.0f, .step = 0.05f, .label = L"Glare Scale", .unit = L"\u00d7", .decimals = 2};
            case SlGlassGlareAngle: return {.min_v = -180.0f, .max_v = 180.0f, .step = 1.0f, .label = L"Glare Light Angle", .unit = L"\u00b0", .decimals = 0};
            default: return {.min_v = 0.0f, .max_v = 1.0f, .step = 0.01f, .label = L"", .unit = L"", .decimals = 2};
        }
    }

    [[nodiscard]] bool slider_overridden(int idx) const noexcept {
        switch (idx) {
            case SlPillAlpha: return draft_.pill_bg_alpha.has_value();
            case SlSettingsAlpha: return draft_.settings_bg_alpha.has_value();
            case SlCardAlpha: return draft_.card_alpha.has_value();
            case SlShadowScale: return draft_.shadow_alpha_scale.has_value();
            case SlPillRadius: return draft_.pill_radius.has_value();
            case SlCardRadius: return draft_.card_radius.has_value();
            case SlButtonRadius: return draft_.button_radius.has_value();
            case SlThumbRadius: return draft_.thumb_radius.has_value();
            case SlGlassBlur: return draft_.glass && draft_.glass->blur.has_value();
            case SlGlassRoundness: return draft_.glass && draft_.glass->roundness.has_value();
            case SlGlassRefraction: return draft_.glass && draft_.glass->refraction.has_value();
            case SlGlassDispersion: return draft_.glass && draft_.glass->dispersion.has_value();
            case SlGlassGlareScale: return draft_.glass && draft_.glass->glare_scale.has_value();
            case SlGlassGlareAngle: return draft_.glass && draft_.glass->glare_angle.has_value();
            default: return false;
        }
    }

    [[nodiscard]] float slider_value(int idx) {
        const Theme& t = resolved_preview();
        switch (idx) {
            case SlPillAlpha: return t.pill_bg_alpha;
            case SlSettingsAlpha: return t.settings_bg_alpha;
            case SlCardAlpha: return t.card_alpha;
            case SlShadowScale: return t.shadow_alpha_scale;
            case SlPillRadius: return t.pill_radius.value_or(28.0f);
            case SlCardRadius: return t.card_radius;
            case SlButtonRadius: return t.button_radius;
            case SlThumbRadius: return t.thumb_radius;
            case SlGlassBlur: return t.glass.blur;
            case SlGlassRoundness: return t.glass.roundness;
            case SlGlassRefraction: return t.glass.refraction;
            case SlGlassDispersion: return t.glass.dispersion;
            case SlGlassGlareScale: return t.glass.glare_scale;
            case SlGlassGlareAngle: return t.glass.glare_angle;
            default: return 0.0f;
        }
    }

    void set_slider_value(int idx, float v) {
        const SliderSpec sp = slider_spec(idx);
        const float snapped = std::round(std::clamp(v, sp.min_v, sp.max_v) / sp.step) * sp.step;
        const auto ensure_glass = [&] -> LiquidGlassSpec& {
            if (!draft_.glass) draft_.glass = LiquidGlassSpec{};
            return *draft_.glass;
        };
        switch (idx) {
            case SlPillAlpha: draft_.pill_bg_alpha = snapped; break;
            case SlSettingsAlpha: draft_.settings_bg_alpha = snapped; break;
            case SlCardAlpha: draft_.card_alpha = snapped; break;
            case SlShadowScale: draft_.shadow_alpha_scale = snapped; break;
            case SlPillRadius: draft_.pill_radius = snapped; break;
            case SlCardRadius: draft_.card_radius = snapped; break;
            case SlButtonRadius: draft_.button_radius = snapped; break;
            case SlThumbRadius: draft_.thumb_radius = snapped; break;
            case SlGlassBlur: ensure_glass().blur = snapped; break;
            case SlGlassRoundness: ensure_glass().roundness = snapped; break;
            case SlGlassRefraction: ensure_glass().refraction = snapped; break;
            case SlGlassDispersion: ensure_glass().dispersion = snapped; break;
            case SlGlassGlareScale: ensure_glass().glare_scale = snapped; break;
            case SlGlassGlareAngle: ensure_glass().glare_angle = snapped; break;
            default: break;
        }
        mark_preview_dirty();
    }

    void reset_slider(int idx) {
        switch (idx) {
            case SlPillAlpha: draft_.pill_bg_alpha.reset(); break;
            case SlSettingsAlpha: draft_.settings_bg_alpha.reset(); break;
            case SlCardAlpha: draft_.card_alpha.reset(); break;
            case SlShadowScale: draft_.shadow_alpha_scale.reset(); break;
            case SlPillRadius: draft_.pill_radius.reset(); break;
            case SlCardRadius: draft_.card_radius.reset(); break;
            case SlButtonRadius: draft_.button_radius.reset(); break;
            case SlThumbRadius: draft_.thumb_radius.reset(); break;
            case SlGlassBlur: if (draft_.glass) draft_.glass->blur.reset(); break;
            case SlGlassRoundness: if (draft_.glass) draft_.glass->roundness.reset(); break;
            case SlGlassRefraction: if (draft_.glass) draft_.glass->refraction.reset(); break;
            case SlGlassDispersion: if (draft_.glass) draft_.glass->dispersion.reset(); break;
            case SlGlassGlareScale: if (draft_.glass) draft_.glass->glare_scale.reset(); break;
            case SlGlassGlareAngle: if (draft_.glass) draft_.glass->glare_angle.reset(); break;
            default: break;
        }
        if (draft_.glass && !draft_.glass->blur && !draft_.glass->roundness && !draft_.glass->refraction &&
            !draft_.glass->dispersion && !draft_.glass->glare_scale && !draft_.glass->glare_angle) {
            draft_.glass.reset();
        }
        mark_preview_dirty();
        sync_switch_targets(true);
    }

    static std::wstring format_slider_value(int idx, float val, bool overridden) {
        const SliderSpec sp = slider_spec(idx);
        std::array<wchar_t, 48> buf{};
        if (sp.decimals == 0) {
            (void)std::swprintf(buf.data(), buf.size(), L"%.0f%ls", val, sp.unit);
        } else if (sp.decimals == 1) {
            (void)std::swprintf(buf.data(), buf.size(), L"%.1f%ls", val, sp.unit);
        } else {
            (void)std::swprintf(buf.data(), buf.size(), L"%.2f%ls", val, sp.unit);
        }
        std::wstring out{buf.data()};
        if (!overridden) out += L" (base)";
        return out;
    }

    void update_slider_from_x(int idx, float x) {
        if (idx < 0 || idx >= SlCount) return;
        const D2D1_RECT_F& r = slider_rects_[static_cast<size_t>(idx)];
        const float track_l = r.left + 8.0f;
        const float track_r = std::max(track_l + 4.0f, r.right - 8.0f);
        const float ratio = std::clamp((x - track_l) / (track_r - track_l), 0.0f, 1.0f);
        const SliderSpec sp = slider_spec(idx);
        set_slider_value(idx, lerp(sp.min_v, sp.max_v, ratio));
        sync_switch_targets(false);
        invalidate();
    }

    // ------------------------------------------------------------------------------------------
    // Drawing
    // ------------------------------------------------------------------------------------------

    void draw_button(Canvas& cv, D2D1_RECT_F r, wchar_t icon_g, std::wstring_view label, int id, int idx = 0, int style = 0) {
        ui::draw_button(cv, target_, sh_.gfx, glass_, hover_.alpha(id, idx), r, icon_g, label, id, idx, style);
    }

    void draw_switch(Canvas& cv, float right_x, float cy, float anim_t, int id, int idx = 0) {
        ui::draw_switch(cv, target_, sh_.gfx, glass_, hover_.alpha(id, idx), anim_t, right_x, cy, id, idx);
    }

    void draw_choice_chip(Canvas& cv, D2D1_RECT_F r, std::wstring_view label, bool on, int id, int idx) {
        const float ha = hover_.alpha(id, idx);
        const float rad = std::min(cv.theme().button_radius, height(r) * 0.5f);
        if (is_liquid_glass(cv.theme())) {
            const bool dark = cv.theme().dark_mode;
            const D2D1_COLOR_F bg = on
                ? with_alpha(cv.theme().primary, dark ? 0.34f : 0.72f)
                : with_alpha(cv.theme().surface_high, dark ? lerp(0.24f, 0.38f, ha) : lerp(0.38f, 0.54f, ha));
            const D2D1_COLOR_F bd = on
                ? with_alpha(cv.theme().primary_hover, 0.80f)
                : with_alpha(hex(0xffffff), dark ? lerp(0.16f, 0.30f, ha) : lerp(0.50f, 0.75f, ha));
            draw_glass_box(cv, r, rad, bg, bd);
        } else {
            const D2D1_COLOR_F bg = on ? cv.theme().primary_container : lerp_color(cv.theme().surface_high, cv.theme().surface_highest, ha);
            const D2D1_COLOR_F bd = on ? cv.theme().primary : cv.theme().outline_variant;
            cv.fill_round(r, rad, bg);
            cv.stroke_round(r, rad, bd);
        }
        const D2D1_COLOR_F fg = on
            ? ((is_liquid_glass(cv.theme()) && !cv.theme().dark_mode) ? cv.theme().on_primary : cv.theme().on_primary_container)
            : cv.theme().on_surface;
        cv.text(label, r, Font::SmallSemi, fg, Align::Center);
        cv.hit(r, id, idx);
    }

    float draw_slider_row(Canvas& cv, float lx, float y, float rw, int idx, std::wstring_view desc) {
        const SliderSpec sp = slider_spec(idx);
        const bool overridden = slider_overridden(idx);
        const float val = slider_value(idx);
        const float ratio = (val - sp.min_v) / std::max(0.0001f, sp.max_v - sp.min_v);

        const float val_w = 112.0f;
        const float reset_w = overridden ? button_width(cv, L"Reset", false, 58.0f) : 0.0f;
        const float right_w = val_w + (overridden ? reset_w + 8.0f : 0.0f);
        const float title_w = std::max(140.0f, rw - right_w - 12.0f);
        const float title_h = text_height(cv, sp.label, Font::BodySemi, title_w, 20.0f);
        const float desc_h = desc.empty() ? 0.0f : text_height(cv, desc, Font::Caption, rw, 16.0f);

        cv.text_wrap(sp.label, rect(lx, y, title_w, title_h), Font::BodySemi, cv.theme().on_surface);
        const std::wstring val_str = format_slider_value(idx, val, overridden);
        cv.text(val_str, rect(lx + rw - right_w, y, val_w, 20.0f), Font::Small,
                overridden ? cv.theme().primary : cv.theme().on_surface_variant, Align::Right);
        if (overridden) {
            draw_button(cv, rect(lx + rw - reset_w, y - 2.0f, reset_w, 24.0f), 0, L"Reset", IdResetSlider, idx, 3);
        }

        float sy = y + title_h + 4.0f;
        if (desc_h > 0.0f) {
            cv.text_wrap(desc, rect(lx, sy, rw, desc_h), Font::Caption, cv.theme().on_surface_variant);
            sy += desc_h + 4.0f;
        }
        const D2D1_RECT_F sr = rect(lx, sy, rw, 22.0f);
        slider_rects_[static_cast<size_t>(idx)] = sr;
        ui::draw_slider(cv, target_, sh_.gfx, glass_, hover_.alpha(IdSlider, idx), sr, ratio, active_slider_ == idx, IdSlider, idx);
        return sy + 28.0f;
    }

    float draw_color_row(Canvas& cv, float lx, float y, float rw, size_t idx, bool is_tag) {
        const Theme& t = resolved_preview();
        const D2D1_COLOR_F resolved_c = is_tag ? t.tag_palette[idx] : t.*kColorDefs[idx].theme_member;
        const bool overridden = is_tag
            ? (idx < draft_.tag_palette.size() && !draft_.tag_palette[idx].empty())
            : !(draft_.*kColorDefs[idx].spec_member).empty();
        TextField& tf = is_tag ? tag_fields_[idx] : color_fields_[idx];
        const std::wstring& placeholder = is_tag ? tag_placeholders_[idx] : color_placeholders_[idx];
        const int swatch_id = is_tag ? IdTagSwatch : IdColorSwatch;
        const int field_id = is_tag ? IdTagField : IdColorField;
        const int reset_id = is_tag ? IdResetTag : IdResetColor;

        const float sw_sz = 30.0f;
        const float field_w = 116.0f;
        const float reset_w = overridden ? button_width(cv, L"Reset", false, 58.0f) : 0.0f;
        const float right_w = field_w + (overridden ? reset_w + 6.0f : 0.0f);
        const float text_x = lx + sw_sz + 10.0f;
        const float text_w = std::max(100.0f, rw - (sw_sz + 10.0f) - right_w - 10.0f);

        std::array<wchar_t, 24> tag_lbl{};
        std::wstring_view label;
        std::wstring_view hint;
        if (is_tag) {
            (void)std::swprintf(tag_lbl.data(), tag_lbl.size(), L"Tag Palette #%zu", idx + 1);
            label = tag_lbl.data();
            hint = L"Cyclic tag pill color";
        } else {
            label = kColorDefs[idx].label;
            hint = kColorDefs[idx].hint;
        }

        const float lh = text_height(cv, label, Font::BodySemi, text_w, 18.0f);
        const float hh = text_height(cv, hint, Font::Caption, text_w, 16.0f);
        const float row_h = std::max(34.0f, lh + hh);

        const float sw_y = y + ((row_h - sw_sz) * 0.5f);
        const D2D1_RECT_F sw_r = rect(lx, sw_y, sw_sz, sw_sz);
        const float sw_rad = std::min(cv.theme().button_radius, 8.0f);
        cv.fill_round(sw_r, sw_rad, resolved_c);
        cv.stroke_round(sw_r, sw_rad, with_alpha(cv.theme().on_surface, 0.35f), 1.0f);
        cv.hit(sw_r, swatch_id, static_cast<int>(idx));

        cv.text_wrap(label, rect(text_x, y, text_w, lh), Font::BodySemi, cv.theme().on_surface);
        cv.text_wrap(hint, rect(text_x, y + lh, text_w, hh), Font::Caption, cv.theme().on_surface_variant);

        const float fx = lx + rw - right_w;
        const float fy = y + ((row_h - 30.0f) * 0.5f);
        const D2D1_RECT_F fr = rect(fx, fy, field_w, 30.0f);
        const float fha = hover_.alpha(field_id, static_cast<int>(idx));
        draw_well(cv, fr, 8.0f, true, tf.focused, false, fha);
        tf.placeholder = placeholder;
        tf.draw(cv, inset(fr, 10.0f, 5.0f), Font::MonoSmall, cv.theme().on_surface, with_alpha(cv.theme().on_surface_variant, 0.65f));
        cv.hit(fr, field_id, static_cast<int>(idx));

        if (overridden) {
            draw_button(cv, rect(fx + field_w + 6.0f, fy + 1.0f, reset_w, 28.0f), 0, L"Reset", reset_id, static_cast<int>(idx), 3);
        }
        return y + row_h + 10.0f;
    }

    void draw_header_bar(Canvas& cv, D2D1_SIZE_F sz) {
        if (!glass_active()) {
            cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, HEADER_H), with_alpha(cv.theme().surface_lowest, 0.78f));
        }
        const std::wstring title = editing_existing_ ? L"Edit Custom Theme" : L"Create Custom Theme";
        const std::wstring sub = editing_existing_
            ? ulp::str::concat_strings(L"Editing ", target_path_.filename().wstring())
            : L"Saves a .json theme into data/themes/ referencing theme.schema.json";
        const D2D1_RECT_F tr = rect(PAD, 10.0f, sz.width - (PAD * 2.0f), 24.0f);
        const D2D1_RECT_F sr = rect(PAD, 34.0f, sz.width - (PAD * 2.0f), 18.0f);
        cv.text(title, tr, Font::Large, cv.theme().on_surface);
        cv.text(sub, sr, Font::Small, cv.theme().on_surface_variant);
        cv.fill(D2D1::RectF(0.0f, HEADER_H, sz.width, HEADER_H + 1.0f),
                with_alpha(is_liquid_glass(cv.theme()) ? hex(0xffffff) : cv.theme().outline_variant, 0.28f));
    }

    void draw_footer_bar(Canvas& cv, D2D1_SIZE_F sz) {
        const float fy = sz.height - FOOTER_H;
        if (!glass_active()) {
            cv.fill(D2D1::RectF(0.0f, fy, sz.width, sz.height), with_alpha(cv.theme().surface_lowest, 0.85f));
        }
        cv.fill(D2D1::RectF(0.0f, fy, sz.width, fy + 1.0f),
                with_alpha(is_liquid_glass(cv.theme()) ? hex(0xffffff) : cv.theme().outline_variant, 0.28f));

        const float by = fy + 12.0f;
        const float bh = 34.0f;
        if (editing_existing_) {
            const float del_w = button_width(cv, L"Delete", true, 98.0f);
            draw_button(cv, rect(PAD, by, del_w, bh), glyph::del, L"Delete", IdDeleteTheme, 0, 2);
        }
        const float save_w = button_width(cv, L"Save & Apply", true, 134.0f);
        const float apply_w = button_width(cv, L"Apply", true, 90.0f);
        const float cancel_w = button_width(cv, L"Cancel", false, 84.0f);

        float rx = sz.width - PAD - save_w;
        draw_button(cv, rect(rx, by, save_w, bh), glyph::check, L"Save & Apply", IdSaveApply, 0, 1);
        rx -= apply_w + 8.0f;
        draw_button(cv, rect(rx, by, apply_w, bh), glyph::refresh, L"Apply", IdApplyOnly, 0, 0);
        rx -= cancel_w + 8.0f;
        draw_button(cv, rect(rx, by, cancel_w, bh), 0, L"Cancel", IdCancel, 0, 3);
    }

    float draw_body(Canvas& cv, float x, float y, float w) {
        const Theme& prev = resolved_preview();
        const float lx = x + 20.0f;
        const float rw = w - 40.0f;

        if (!error_msg_.empty()) {
            const float eh = std::max(38.0f, text_height(cv, error_msg_, Font::Small, rw - 24.0f, 20.0f) + 18.0f);
            cv.fill_round(rect(x, y, w, eh), cv.theme().card_radius, with_alpha(cv.theme().error_container, 0.45f));
            cv.stroke_round(rect(x, y, w, eh), cv.theme().card_radius, cv.theme().error);
            cv.icon(glyph::warning, rect(x + 12.0f, y, 20.0f, eh), cv.theme().error, Font::IconSmall);
            cv.text_wrap(error_msg_, rect(x + 36.0f, y + 9.0f, rw - 16.0f, eh - 18.0f), Font::Small, cv.theme().error);
            y += eh + CARD_GAP;
        }

        // 1. Identity & Base Preset
        {
            const CardHeader hd = make_header(cv, rw, L"Identity & Base", cv.theme().on_surface,
                                              L"Name, base palette preset, dark/light mode, and window surface style");
            const std::wstring stem_w = editing_existing_
                ? target_path_.filename().wstring()
                : to_wide(ulp::str::concat_strings(sanitize_theme_stem(draft_.name.empty() ? "Custom Theme" : draft_.name), ".json"));
            const std::wstring file_note = ulp::str::concat_strings(L"File: ", stem_w);
            const float file_w = std::min(rw * 0.45f, cv.measure(file_note, Font::MonoSmall) + 20.0f);
            const float name_w = std::max(160.0f, rw - file_w - 8.0f);

            Flow base_chips{8.0f, 8.0f};
            for (size_t i = 0; i < kBaseOptions.size(); ++i) {
                const bool active = draft_.base == kBaseOptions[i].value || (draft_.base.empty() && kBaseOptions[i].value == (prev.is_liquid_glass ? "liquid-glass" : "dark"));
                const float cw = cv.measure(kBaseOptions[i].label, Font::SmallSemi) + 24.0f;
                base_chips.add(cw, 28.0f, [&, i, active](D2D1_RECT_F r) {
                    draw_choice_chip(cv, r, kBaseOptions[i].label, active, IdBaseChip, static_cast<int>(i));
                });
            }

            Flow mode_chips{8.0f, 8.0f};
            for (size_t i = 0; i < kModeOptions.size(); ++i) {
                const bool active = draft_.mode == kModeOptions[i].value;
                const float cw = cv.measure(kModeOptions[i].label, Font::SmallSemi) + 24.0f;
                mode_chips.add(cw, 28.0f, [&, i, active](D2D1_RECT_F r) {
                    draw_choice_chip(cv, r, kModeOptions[i].label, active, IdModeChip, static_cast<int>(i));
                });
            }

            Flow pill_bk{8.0f, 8.0f};
            Flow set_bk{8.0f, 8.0f};
            if (!prev.is_liquid_glass) {
                for (size_t i = 0; i < kBackdropOptions.size(); ++i) {
                    const float cw = cv.measure(kBackdropOptions[i].label, Font::SmallSemi) + 22.0f;
                    const bool ap = draft_.pill_backdrop == kBackdropOptions[i].value;
                    const bool as = draft_.settings_backdrop == kBackdropOptions[i].value;
                    pill_bk.add(cw, 28.0f, [&, i, ap](D2D1_RECT_F r) {
                        draw_choice_chip(cv, r, kBackdropOptions[i].label, ap, IdPillBackdropChip, static_cast<int>(i));
                    });
                    set_bk.add(cw, 28.0f, [&, i, as](D2D1_RECT_F r) {
                        draw_choice_chip(cv, r, kBackdropOptions[i].label, as, IdSettingsBackdropChip, static_cast<int>(i));
                    });
                }
            }

            float cur_y = y + 16.0f + hd.h + 12.0f;
            const float name_y = cur_y;
            cur_y += 34.0f + 12.0f;
            const float base_lbl_y = cur_y;
            cur_y += 22.0f;
            const float base_y = cur_y;
            cur_y += base_chips.height(rw) + 12.0f;
            const float mode_lbl_y = cur_y;
            cur_y += 22.0f;
            const float mode_y = cur_y;
            cur_y += mode_chips.height(rw) + 12.0f;
            const float glass_y = cur_y;
            cur_y += 32.0f;
            const float round_y = cur_y;
            cur_y += 32.0f;
            float pbk_lbl_y = 0.0f;
            float pbk_y = 0.0f;
            float sbk_lbl_y = 0.0f;
            float sbk_y = 0.0f;
            if (!prev.is_liquid_glass) {
                cur_y += 6.0f;
                pbk_lbl_y = cur_y;
                cur_y += 22.0f;
                pbk_y = cur_y;
                cur_y += pill_bk.height(rw) + 10.0f;
                sbk_lbl_y = cur_y;
                cur_y += 22.0f;
                sbk_y = cur_y;
                cur_y += set_bk.height(rw);
            }
            const float h = (cur_y + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, h, CARD_GAP);

            draw_header(cv, lx, y, rw, hd);
            const D2D1_RECT_F nfr = rect(lx, name_y, name_w, 34.0f);
            draw_well(cv, nfr, 8.0f, true, name_field_.focused, false, hover_.alpha(IdNameField));
            name_field_.draw(cv, inset(nfr, 10.0f, 7.0f), Font::Body, cv.theme().on_surface, cv.theme().outline);
            cv.hit(nfr, IdNameField);

            const D2D1_RECT_F fdr = rect(lx + name_w + 8.0f, name_y, rw - name_w - 8.0f, 34.0f);
            draw_well(cv, fdr, 8.0f);
            text_middle(cv, file_note, inset(fdr, 10.0f, 0.0f), Font::MonoSmall, cv.theme().on_surface_variant);

            cv.text(L"Base Preset", rect(lx, base_lbl_y, rw, 20.0f), Font::SmallSemi, cv.theme().on_surface_variant);
            base_chips.draw(lx, base_y, rw);
            cv.text(L"Color Mode", rect(lx, mode_lbl_y, rw, 20.0f), Font::SmallSemi, cv.theme().on_surface_variant);
            mode_chips.draw(lx, mode_y, rw);

            cv.text(L"Enable Liquid Glass shader", rect(lx, glass_y + 2.0f, rw - 54.0f, 22.0f), Font::BodySemi, cv.theme().on_surface);
            draw_switch(cv, lx + rw, glass_y + 14.0f, sw_glass_.value, IdToggleLiquidGlass);

            cv.text(L"Rounded window corners (DWM)", rect(lx, round_y + 2.0f, rw - 54.0f, 22.0f), Font::BodySemi, cv.theme().on_surface);
            draw_switch(cv, lx + rw, round_y + 14.0f, sw_round_.value, IdToggleRoundCorners);

            if (!prev.is_liquid_glass) {
                cv.text(L"Search Pill DWM Backdrop", rect(lx, pbk_lbl_y, rw, 20.0f), Font::SmallSemi, cv.theme().on_surface_variant);
                pill_bk.draw(lx, pbk_y, rw);
                cv.text(L"Settings & Editor DWM Backdrop", rect(lx, sbk_lbl_y, rw, 20.0f), Font::SmallSemi, cv.theme().on_surface_variant);
                set_bk.draw(lx, sbk_y, rw);
            }
            y = next_y;
        }

        // 2. Opacity & Shadow
        {
            const CardHeader hd = make_header(cv, rw, L"Opacity & Shadow", cv.theme().on_surface,
                                              L"Surface translucency for the pill, windows, cards, and custom drop shadow");
            float cur_y = y + 16.0f + hd.h + 12.0f;
            const float y0 = cur_y;
            // Measure height cleanly by simulating or computing fixed slider heights cleanly:
            const auto sim_slider_h = [&](int idx, std::wstring_view desc) {
                const float th = text_height(cv, slider_spec(idx).label, Font::BodySemi, std::max(140.0f, rw - 190.0f), 20.0f);
                const float dh = desc.empty() ? 0.0f : text_height(cv, desc, Font::Caption, rw, 16.0f) + 4.0f;
                return th + 4.0f + dh + 28.0f;
            };
            cur_y += sim_slider_h(SlPillAlpha, L"");
            cur_y += sim_slider_h(SlSettingsAlpha, L"");
            cur_y += sim_slider_h(SlCardAlpha, L"");
            cur_y += sim_slider_h(SlShadowScale, L"Scales custom drop shadows on cards, knobs, and rounded-radius pill");
            const float h = (cur_y + 12.0f) - y;
            const float next_y = card_begin(cv, x, y, w, h, CARD_GAP);
            draw_header(cv, lx, y, rw, hd);
            float sy = y0;
            sy = draw_slider_row(cv, lx, sy, rw, SlPillAlpha, L"");
            sy = draw_slider_row(cv, lx, sy, rw, SlSettingsAlpha, L"");
            sy = draw_slider_row(cv, lx, sy, rw, SlCardAlpha, L"");
            (void)draw_slider_row(cv, lx, sy, rw, SlShadowScale, L"Scales custom drop shadows on cards, knobs, and rounded-radius pill");
            y = next_y;
        }

        // 3. Corner Radii
        {
            const CardHeader hd = make_header(cv, rw, L"Corner Radii", cv.theme().on_surface,
                                              L"Customize search pill radius and internal card, button, and thumbnail corners");
            const auto sim_slider_h = [&](int idx, std::wstring_view desc) {
                const float th = text_height(cv, slider_spec(idx).label, Font::BodySemi, std::max(140.0f, rw - 190.0f), 20.0f);
                const float dh = desc.empty() ? 0.0f : text_height(cv, desc, Font::Caption, rw, 16.0f) + 4.0f;
                return th + 4.0f + dh + 28.0f;
            };
            float cur_y = y + 16.0f + hd.h + 12.0f;
            const float toggle_y = cur_y;
            cur_y += 38.0f;
            const float pr_y = cur_y;
            if (draft_.pill_radius.has_value()) cur_y += sim_slider_h(SlPillRadius, L"Overrides OS DWM rounding with custom radius + outer shadow");
            const float cr_y = cur_y;
            cur_y += sim_slider_h(SlCardRadius, L"");
            cur_y += sim_slider_h(SlButtonRadius, L"Also scales input wells and choice chips");
            cur_y += sim_slider_h(SlThumbRadius, L"Used for search result thumbnails");
            const float h = (cur_y + 12.0f) - y;
            const float next_y = card_begin(cv, x, y, w, h, CARD_GAP);

            draw_header(cv, lx, y, rw, hd);
            cv.text(L"Custom pill corner radius", rect(lx, toggle_y, rw - 54.0f, 18.0f), Font::BodySemi, cv.theme().on_surface);
            cv.text(L"Off uses native Windows corner preference", rect(lx, toggle_y + 18.0f, rw - 54.0f, 16.0f),
                    Font::Caption, cv.theme().on_surface_variant);
            draw_switch(cv, lx + rw, toggle_y + 14.0f, sw_pill_rad_.value, IdTogglePillRadius);

            float sy = pr_y;
            if (draft_.pill_radius.has_value()) {
                sy = draw_slider_row(cv, lx, sy, rw, SlPillRadius, L"Overrides OS DWM rounding with custom radius + outer shadow");
            }
            sy = draw_slider_row(cv, lx, cr_y, rw, SlCardRadius, L"");
            sy = draw_slider_row(cv, lx, sy, rw, SlButtonRadius, L"Also scales input wells and choice chips");
            (void)draw_slider_row(cv, lx, sy, rw, SlThumbRadius, L"Used for search result thumbnails");
            y = next_y;
        }

        // 4. Liquid Glass Material (when active)
        if (prev.is_liquid_glass) {
            const float reset_w = button_width(cv, L"Reset Glass", true, 114.0f);
            const CardHeader hd = make_header(cv, rw, L"Liquid Glass Material", cv.theme().on_surface,
                                              L"Desktop refraction, chromatic dispersion, blur, and specular glare",
                                              draft_.glass ? reset_w : 0.0f, 30.0f);
            const auto sim_slider_h = [&](int idx) {
                return text_height(cv, slider_spec(idx).label, Font::BodySemi, std::max(140.0f, rw - 190.0f), 20.0f) + 32.0f;
            };
            float cur_y = y + 16.0f + hd.h + 12.0f;
            const float sy0 = cur_y;
            for (int idx = SlGlassBlur; idx <= SlGlassGlareAngle; ++idx) cur_y += sim_slider_h(idx);
            const float h = (cur_y + 12.0f) - y;
            const float next_y = card_begin(cv, x, y, w, h, CARD_GAP);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            if (draft_.glass) {
                draw_button(cv, side, glyph::refresh, L"Reset Glass", IdResetGlass);
            }
            float sy = sy0;
            for (int idx = SlGlassBlur; idx <= SlGlassGlareAngle; ++idx) {
                sy = draw_slider_row(cv, lx, sy, rw, idx, L"");
            }
            y = next_y;
        }

        // 5. Colors & Tag Palette
        {
            const std::wstring toggle_lbl = show_advanced_colors_ ? L"Hide Advanced & Tags" : L"Advanced & Tags (+21)";
            const float adv_w = button_width(cv, toggle_lbl, false, 160.0f);
            const CardHeader hd = make_header(cv, rw, L"Colors", cv.theme().on_surface,
                                              L"Click a swatch to pick a color, or enter #RRGGBB / #RRGGBBAA hex", adv_w, 30.0f);
            const auto measure_color_row = [&](std::wstring_view label, std::wstring_view hint) {
                const float tw = std::max(100.0f, rw - 40.0f - 180.0f);
                return std::max(34.0f, text_height(cv, label, Font::BodySemi, tw, 18.0f) + text_height(cv, hint, Font::Caption, tw, 16.0f)) + 10.0f;
            };

            float cur_y = y + 16.0f + hd.h + 14.0f;
            const float rows_y0 = cur_y;
            for (const ColorFieldDef& cd : kColorDefs) {
                if (!cd.advanced || show_advanced_colors_) {
                    cur_y += measure_color_row(cd.label, cd.hint);
                }
            }
            if (show_advanced_colors_) {
                cur_y += 28.0f + (static_cast<float>(tag_fields_.size()) * measure_color_row(L"Tag Palette #8", L"Cyclic tag pill color"));
            }
            const float h = (cur_y + 10.0f) - y;
            const float next_y = card_begin(cv, x, y, w, h, CARD_GAP);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_button(cv, side, 0, toggle_lbl, IdToggleAdvancedColors);

            float ry = rows_y0;
            for (size_t i = 0; i < kColorDefs.size(); ++i) {
                if (!kColorDefs[i].advanced || show_advanced_colors_) {
                    ry = draw_color_row(cv, lx, ry, rw, i, false);
                }
            }
            if (show_advanced_colors_) {
                ry += 4.0f;
                cv.text(L"Tag Palette", rect(lx, ry, rw, 22.0f), Font::BodySemi, cv.theme().on_surface_variant);
                ry += 24.0f;
                for (size_t i = 0; i < tag_fields_.size(); ++i) {
                    ry = draw_color_row(cv, lx, ry, rw, i, true);
                }
            }
            y = next_y;
        }

        return y;
    }

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        const bool glass = glass_active();
        const bool need_scene = glass && (!cv.dirty_clip() || cv.dirty_clip()->top < HEADER_H + 24.0f ||
                                          cv.dirty_clip()->bottom > sz.height - FOOTER_H - 24.0f);
        const bool has_scene = need_scene && glass_.begin_scene(target_, sh_.gfx);
        if (glass) {
            if (!glass_.draw_desktop_glass(sh_.desktop_dupl, cv, target_, sh_.gfx, hwnd_,
                                           D2D1::RectF(0.0f, 0.0f, sz.width, sz.height),
                                           make_desktop_glass_style(cv.theme(), 0.0f, true))) {
                cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), with_alpha(cv.theme().bg, cv.theme().settings_bg_alpha));
            }
        } else {
            cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), with_alpha(cv.theme().bg, cv.theme().settings_bg_alpha));
        }

        if (!glass) {
            draw_header_bar(cv, sz);
        }

        const D2D1_RECT_F view = D2D1::RectF(0.0f, HEADER_H + 1.0f, sz.width, std::max(HEADER_H + 40.0f, sz.height - FOOTER_H));
        cv.push_clip(view);

        const float cw = std::min(720.0f, sz.width - (PAD * 2.0f));
        const float cx = std::round((sz.width - cw) * 0.5f);
        const float end_y = draw_body(cv, cx, view.top + PAD - scroll_, cw);
        content_h_ = (end_y + scroll_ - view.top) + PAD;
        cv.pop_clip();

        const float ms = max_scroll();
        if (ms > 0.0f) {
            const float track_h = height(view) - 8.0f;
            const float thumb_h = std::max(28.0f, track_h * height(view) / content_h_);
            const float ty = view.top + 4.0f + ((track_h - thumb_h) * (scroll_ / ms));
            const float ha = dragging_scroll_ ? 1.0f : hover_.alpha(IdScrollbar);
            const D2D1_RECT_F tr = D2D1::RectF(view.right - lerp(7.0f, 9.0f, ha), ty, view.right - 3.0f, ty + thumb_h);
            cv.fill_round(tr, 3.0f, with_alpha(cv.theme().on_surface_variant, lerp(0.28f, 0.55f, ha)));
            cv.hit(D2D1::RectF(view.right - 12.0f, view.top, view.right, view.bottom), IdScrollbar);
            scroll_thumb_ = tr;
        }

        if (!glass) {
            draw_footer_bar(cv, sz);
        }

        if (has_scene) glass_.end_scene(target_);
        if (need_scene) {
            glass_.draw_scene_glass(cv, target_, sh_.gfx, D2D1::RectF(0.0f, 0.0f, sz.width, HEADER_H),
                                    make_overlay_glass_style(cv.theme(), 0.0f, 1.8f));
            draw_header_bar(cv, sz);
            glass_.draw_scene_glass(cv, target_, sh_.gfx, D2D1::RectF(0.0f, sz.height - FOOTER_H, sz.width, sz.height),
                                    make_overlay_glass_style(cv.theme(), 0.0f, 1.8f));
            draw_footer_bar(cv, sz);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Input, animation & frame rendering
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] float view_height() const noexcept {
        return std::max(1.0f, target_.size().height - HEADER_H - FOOTER_H - 1.0f);
    }

    [[nodiscard]] float max_scroll() const noexcept {
        return std::max(0.0f, content_h_ - view_height());
    }

    bool step_anim() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_anim_).count(), 0.0f, 0.05f);
        last_anim_ = now;
        const float ms = max_scroll();
        target_scroll_ = std::clamp(target_scroll_, 0.0f, ms);
        const float diff = target_scroll_ - scroll_;
        bool active = false;
        if (std::abs(diff) < 0.35f) {
            scroll_ = target_scroll_;
        } else {
            scroll_ = std::clamp(scroll_ + (diff * std::min(1.0f, 18.0f * dt)), 0.0f, ms);
            active = true;
        }
        active |= hover_.step(dt);
        active |= sw_glass_.step(dt);
        active |= sw_round_.step(dt);
        active |= sw_pill_rad_.step(dt);
        active |= sw_adv_.step(dt);
        return active;
    }

    void start_anim() noexcept {
        if (!anim_active_) last_anim_ = std::chrono::steady_clock::now();
        anim_active_ = true;
        if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void render() {
        if (rendering_ || (!visible() && !sh_.test_mode)) return;
        if (!target_.ready()) {
            target_.rebuild();
            if (!target_.ready()) return;
        }
        rendering_ = true;

        const bool anim = step_anim();
        if (glass_active()) {
            (void)glass_.poll_desktop(sh_.desktop_dupl, sh_.gfx, hwnd_);
        }
        const bool request_full = std::exchange(full_dirty_, false);
        dirty_dips_scratch_.clear();
        const bool ok = target_.render_frame(request_full || anim || dragging_scroll_ || active_slider_ >= 0,
                                             dirty_dips_scratch_, !in_sizemove_, [&](std::optional<D2D1_RECT_F> clip, bool full_frame) {
            if (full_frame) {
                hits_.clear();
                Canvas cv{target_, sh_.fonts, hits_, sh_.theme, clip};
                draw(cv);
            } else {
                scratch_hits_.clear();
                Canvas cv{target_, sh_.fonts, scratch_hits_, sh_.theme, clip};
                draw(cv);
            }
        });
        glass_.end_frame();
        if (!ok) {
            target_.rebuild();
            full_dirty_ = true;
        }
        rendering_ = false;
        anim_active_ = visible() && anim;
    }

    void set_active_field(TextField* tf, float click_x) {
        if (active_field_ && active_field_ != tf) {
            commit_field(active_field_);
            active_field_->focused = false;
        }
        active_field_ = tf;
        if (active_field_) {
            active_field_->focused = true;
            active_field_->caret_on = true;
            std::vector<Hit> tmp_hits;
            Canvas cv{target_, sh_.fonts, tmp_hits, sh_.theme};
            active_field_->click(cv, click_x, GetKeyState(VK_SHIFT) < 0);
            SetTimer(hwnd_, TIMER_CARET, 530, nullptr);
        } else {
            KillTimer(hwnd_, TIMER_CARET);
        }
        invalidate();
    }

    void on_lbutton_down(D2D1_POINT_2F p) {
        if (in_modal_) return;
        const Hit* h = hit_test(hits_, p);
        const int id = h ? h->id : IdNone;
        const int idx = h ? h->index : 0;

        if (id != IdNameField && id != IdColorField && id != IdTagField && active_field_) {
            commit_field(active_field_);
            active_field_->focused = false;
            active_field_ = nullptr;
            KillTimer(hwnd_, TIMER_CARET);
        }

        switch (id) {
            case IdNameField:
                set_active_field(&name_field_, p.x);
                return;
            case IdColorField:
                if (idx >= 0 && static_cast<size_t>(idx) < color_fields_.size()) {
                    set_active_field(&color_fields_[static_cast<size_t>(idx)], p.x);
                }
                return;
            case IdTagField:
                if (idx >= 0 && static_cast<size_t>(idx) < tag_fields_.size()) {
                    set_active_field(&tag_fields_[static_cast<size_t>(idx)], p.x);
                }
                return;
            case IdBaseChip:
                if (idx >= 0 && static_cast<size_t>(idx) < kBaseOptions.size()) {
                    draft_.base = std::string{kBaseOptions[static_cast<size_t>(idx)].value};
                    if (draft_.base == "liquid-glass") {
                        draft_.liquid_glass = true;
                    } else {
                        draft_.liquid_glass.reset();
                    }
                    mark_preview_dirty();
                    sync_switch_targets(true);
                }
                break;
            case IdModeChip:
                if (idx >= 0 && static_cast<size_t>(idx) < kModeOptions.size()) {
                    draft_.mode = std::string{kModeOptions[static_cast<size_t>(idx)].value};
                    mark_preview_dirty();
                }
                break;
            case IdToggleLiquidGlass: {
                const bool cur = resolved_preview().is_liquid_glass;
                draft_.liquid_glass = !cur;
                mark_preview_dirty();
                sync_switch_targets(true);
                break;
            }
            case IdToggleRoundCorners: {
                const bool cur = resolved_preview().round_corners;
                draft_.round_corners = !cur;
                mark_preview_dirty();
                sync_switch_targets(true);
                break;
            }
            case IdPillBackdropChip:
                if (idx >= 0 && static_cast<size_t>(idx) < kBackdropOptions.size()) {
                    draft_.pill_backdrop = std::string{kBackdropOptions[static_cast<size_t>(idx)].value};
                    mark_preview_dirty();
                }
                break;
            case IdSettingsBackdropChip:
                if (idx >= 0 && static_cast<size_t>(idx) < kBackdropOptions.size()) {
                    draft_.settings_backdrop = std::string{kBackdropOptions[static_cast<size_t>(idx)].value};
                    mark_preview_dirty();
                }
                break;
            case IdTogglePillRadius:
                if (draft_.pill_radius.has_value()) {
                    draft_.pill_radius.reset();
                } else {
                    draft_.pill_radius = 28.0f;
                }
                mark_preview_dirty();
                sync_switch_targets(true);
                break;
            case IdSlider:
                if (idx >= 0 && idx < SlCount) {
                    active_slider_ = idx;
                    SetCapture(hwnd_);
                    update_slider_from_x(idx, p.x);
                }
                return;
            case IdResetSlider:
                reset_slider(idx);
                break;
            case IdResetGlass:
                draft_.glass.reset();
                mark_preview_dirty();
                break;
            case IdColorSwatch:
                if (idx >= 0 && static_cast<size_t>(idx) < kColorDefs.size()) {
                    pick_color_native(static_cast<size_t>(idx), false);
                }
                return;
            case IdTagSwatch:
                if (idx >= 0 && static_cast<size_t>(idx) < tag_fields_.size()) {
                    pick_color_native(static_cast<size_t>(idx), true);
                }
                return;
            case IdResetColor:
                if (idx >= 0 && static_cast<size_t>(idx) < kColorDefs.size()) {
                    draft_.*kColorDefs[static_cast<size_t>(idx)].spec_member = "";
                    color_fields_[static_cast<size_t>(idx)].set({});
                    mark_preview_dirty();
                }
                break;
            case IdResetTag:
                if (idx >= 0 && static_cast<size_t>(idx) < tag_fields_.size()) {
                    if (static_cast<size_t>(idx) < draft_.tag_palette.size()) {
                        draft_.tag_palette[static_cast<size_t>(idx)].clear();
                        trim_empty_tag_palette();
                    }
                    tag_fields_[static_cast<size_t>(idx)].set({});
                    mark_preview_dirty();
                }
                break;
            case IdToggleAdvancedColors:
                show_advanced_colors_ = !show_advanced_colors_;
                sync_switch_targets(true);
                break;
            case IdSaveApply:
                save_and_apply();
                return;
            case IdApplyOnly:
                apply_preview_only();
                return;
            case IdDeleteTheme:
                delete_current_theme();
                return;
            case IdCancel:
                hide(true);
                return;
            case IdScrollbar:
                if (contains(scroll_thumb_, p)) {
                    dragging_scroll_ = true;
                    drag_y_ = p.y;
                    drag_scroll_start_ = scroll_;
                    SetCapture(hwnd_);
                }
                return;
            default:
                break;
        }
        invalidate();
    }

    void on_key(UINT vk) {
        if (active_field_) {
            if (vk == VK_RETURN) {
                commit_field(active_field_);
                active_field_->focused = false;
                active_field_ = nullptr;
                KillTimer(hwnd_, TIMER_CARET);
                invalidate();
                return;
            }
            if (vk == VK_ESCAPE) {
                load_draft_into_fields();
                active_field_->focused = false;
                active_field_ = nullptr;
                KillTimer(hwnd_, TIMER_CARET);
                invalidate();
                return;
            }
            bool handled = false;
            if (active_field_->on_key(hwnd_, vk, handled)) {
                for (size_t i = 0; i < color_fields_.size(); ++i) {
                    if (active_field_ == &color_fields_[i]) apply_hex_field_value(i, true);
                }
                for (size_t i = 0; i < tag_fields_.size(); ++i) {
                    if (active_field_ == &tag_fields_[i]) apply_tag_field_value(i, true);
                }
            }
            active_field_->caret_on = true;
            invalidate();
            return;
        }
        if (vk == VK_ESCAPE) hide(true);
    }

    void on_char(wchar_t c) {
        if (!active_field_) return;
        const bool is_hex_field = active_field_ != &name_field_;
        if (is_hex_field) {
            const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L'#';
            if (!ok) return;
        }
        if (active_field_->on_char(c)) {
            if (active_field_ == &name_field_) {
                draft_.name = to_utf8(name_field_.text);
            } else {
                for (size_t i = 0; i < color_fields_.size(); ++i) {
                    if (active_field_ == &color_fields_[i]) apply_hex_field_value(i, true);
                }
                for (size_t i = 0; i < tag_fields_.size(); ++i) {
                    if (active_field_ == &tag_fields_[i]) apply_tag_field_value(i, true);
                }
            }
            active_field_->caret_on = true;
            invalidate();
        }
    }

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<ThemeEditorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (auto* self = reinterpret_cast<ThemeEditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            return self->handle(msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        const auto to_dip = [this](LPARAM l) -> D2D1_POINT_2F {
            const float s = target_.scale();
            return {.x = GET_X_LPARAM(l) / s, .y = GET_Y_LPARAM(l) / s};
        };
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd_, &ps);
                if (visible() || sh_.test_mode) render();
                EndPaint(hwnd_, &ps);
                if (visible() && anim_active_) {
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
            case WM_MOVE:
                if (glass_active() && visible()) {
                    full_dirty_ = true;
                    invalidate();
                }
                return 0;
            case WM_ENTERSIZEMOVE:
                in_sizemove_ = true;
                if (glass_active()) glass_.set_sizemove(true, sh_.desktop_dupl, sh_.gfx);
                return 0;
            case WM_EXITSIZEMOVE:
                in_sizemove_ = false;
                glass_.set_sizemove(false, sh_.desktop_dupl, sh_.gfx);
                if (sh_.gfx.adapter_stale(hwnd_)) {
                    sh_.gfx.recover(hwnd_);
                    target_.rebuild();
                }
                full_dirty_ = true;
                render();
                return 0;
            case WM_DISPLAYCHANGE:
                if (sh_.gfx.adapter_stale(hwnd_)) {
                    sh_.gfx.recover(hwnd_);
                    target_.rebuild();
                }
                invalidate();
                return 0;
            case WM_DPICHANGED: {
                full_dirty_ = true;
                icons_.apply(hwnd_, inst_, 101);
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                target_.sync_dpi();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                const float s = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
                mmi->ptMinTrackSize = {.x = static_cast<LONG>(460.0f * s), .y = static_cast<LONG>(520.0f * s)};
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd_, &pt);
                    const Hit* h = hit_test(hits_, {.x = static_cast<float>(pt.x) / target_.scale(),
                                                     .y = static_cast<float>(pt.y) / target_.scale(),});
                    const int id = h ? h->id : IdNone;
                    const bool ibeam = id == IdNameField || id == IdColorField || id == IdTagField;
                    SetCursor(ibeam ? cursor_ibeam_ : (id != IdNone ? cursor_hand_ : cursor_arrow_));
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: {
                if (!tracking_) {
                    TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd_, .dwHoverTime = 0};
                    TrackMouseEvent(&tme);
                    tracking_ = true;
                }
                const D2D1_POINT_2F p = to_dip(lp);
                if (active_slider_ >= 0) {
                    if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                        active_slider_ = -1;
                        ReleaseCapture();
                    } else {
                        update_slider_from_x(active_slider_, p.x);
                        return 0;
                    }
                }
                if (dragging_scroll_) {
                    if ((static_cast<uint16_t>(GetAsyncKeyState(VK_LBUTTON)) & 0x8000u) == 0u) {
                        dragging_scroll_ = false;
                        ReleaseCapture();
                    } else {
                        const float track = view_height() - 8.0f - height(scroll_thumb_);
                        if (track > 0.0f) {
                            scroll_ = target_scroll_ = std::clamp(drag_scroll_start_ + ((p.y - drag_y_) / track * max_scroll()), 0.0f, max_scroll());
                        }
                        invalidate();
                        return 0;
                    }
                }
                const Hit* h = hit_test(hits_, p);
                const int id = h ? h->id : IdNone;
                const int idx = h ? h->index : 0;
                if (id != raw_hover_id_ || idx != raw_hover_index_) {
                    raw_hover_id_ = id;
                    raw_hover_index_ = idx;
                    const bool animate = visible() && !sh_.test_mode;
                    if (hover_.set(id, idx, animate)) {
                        if (animate) {
                            start_anim();
                        } else {
                            invalidate();
                        }
                    }
                }
                return 0;
            }
            case WM_MOUSELEAVE: {
                tracking_ = false;
                if (raw_hover_id_ != IdNone || hover_.id != IdNone) {
                    raw_hover_id_ = IdNone;
                    raw_hover_index_ = 0;
                    const bool animate = visible() && !sh_.test_mode;
                    hover_.set(IdNone, 0, animate);
                    if (animate) {
                        start_anim();
                    } else {
                        invalidate();
                    }
                }
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                on_lbutton_down(to_dip(lp));
                return 0;
            case WM_LBUTTONUP:
                if (active_slider_ >= 0 || dragging_scroll_) {
                    active_slider_ = -1;
                    dragging_scroll_ = false;
                    ReleaseCapture();
                    invalidate();
                }
                return 0;
            case WM_CAPTURECHANGED:
                active_slider_ = -1;
                dragging_scroll_ = false;
                return 0;
            case WM_MOUSEWHEEL:
                target_scroll_ = std::clamp(target_scroll_ - (GET_WHEEL_DELTA_WPARAM(wp) * 0.9f), 0.0f, max_scroll());
                start_anim();
                return 0;
            case WM_KEYDOWN:
                on_key(static_cast<UINT>(wp));
                return 0;
            case WM_CHAR:
                on_char(static_cast<wchar_t>(wp));
                return 0;
            case WM_TIMER:
                if (wp == TIMER_CARET && active_field_) {
                    active_field_->caret_on = !active_field_->caret_on;
                    invalidate();
                    return 0;
                }
                if (wp == TIMER_GLASS && glass_active() && visible() && glass_.poll_desktop(sh_.desktop_dupl, sh_.gfx, hwnd_)) {
                    invalidate();
                    return 0;
                }
                return 0;
            case WM_CLOSE:
                hide(true);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    Shared& sh_;
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    WindowTarget target_;
    LiquidGlassWindow glass_;
    WindowIcons icons_;
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_ibeam_ = nullptr;

    std::vector<Hit> hits_;
    std::vector<Hit> scratch_hits_;
    std::vector<D2D1_RECT_F> dirty_dips_scratch_;
    bool rendering_ = false;
    bool full_dirty_ = true;
    bool tracking_ = false;
    bool anim_active_ = false;
    bool in_sizemove_ = false;
    bool in_modal_ = false;

    float scroll_ = 0.0f;
    float target_scroll_ = 0.0f;
    float content_h_ = 0.0f;
    std::chrono::steady_clock::time_point last_anim_ = std::chrono::steady_clock::now();
    bool dragging_scroll_ = false;
    float drag_y_ = 0.0f;
    float drag_scroll_start_ = 0.0f;
    D2D1_RECT_F scroll_thumb_{};

    HoverTracker hover_;
    int raw_hover_id_ = IdNone;
    int raw_hover_index_ = 0;

    AnimFloat sw_glass_;
    AnimFloat sw_round_;
    AnimFloat sw_pill_rad_;
    AnimFloat sw_adv_;

    int active_slider_ = -1;
    std::array<D2D1_RECT_F, SlCount> slider_rects_{};

    ThemeSpec draft_{};
    Theme preview_ = theme_dark();
    bool preview_dirty_ = true;
    bool editing_existing_ = false;
    fs::path target_path_;
    bool preview_applied_ = false;
    bool show_advanced_colors_ = false;
    std::wstring error_msg_;

    TextField name_field_;
    std::array<TextField, 23> color_fields_{};
    std::array<std::wstring, 23> color_placeholders_{};
    std::array<TextField, 8> tag_fields_{};
    std::array<std::wstring, 8> tag_placeholders_{};
    TextField* active_field_ = nullptr;
};

} // namespace usyseek::ui
