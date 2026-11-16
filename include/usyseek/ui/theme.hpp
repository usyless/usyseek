#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace usyseek::ui {

[[nodiscard]] inline D2D1_COLOR_F hex(uint32_t rgb, float a = 1.0f) noexcept {
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16u) & 0xFFu) / 255.0f,
        static_cast<float>((rgb >> 8u) & 0xFFu) / 255.0f,
        static_cast<float>(rgb & 0xFFu) / 255.0f,
        a);
}

[[nodiscard]] inline D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) noexcept {
    c.a = a;
    return c;
}

[[nodiscard]] inline D2D1_COLOR_F lerp_color(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) noexcept {
    const float k = std::clamp(t, 0.0f, 1.0f);
    return {
        .r = a.r + ((b.r - a.r) * k),
        .g = a.g + ((b.g - a.g) * k),
        .b = a.b + ((b.b - a.b) * k),
        .a = a.a + ((b.a - a.a) * k),
    };
}

[[nodiscard]] inline COLORREF to_colorref(D2D1_COLOR_F c) noexcept {
    const auto byte = [](float v) -> BYTE {
        return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return RGB(byte(c.r), byte(c.g), byte(c.b));
}

struct Hsl {
    float h = 0.0f; // [0, 360)
    float s = 0.0f; // [0, 1]
    float l = 0.0f; // [0, 1]
};

[[nodiscard]] inline Hsl rgb_to_hsl(D2D1_COLOR_F c) noexcept {
    const float r = std::clamp(c.r, 0.0f, 1.0f);
    const float g = std::clamp(c.g, 0.0f, 1.0f);
    const float b = std::clamp(c.b, 0.0f, 1.0f);
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d = mx - mn;
    Hsl out{};
    out.l = (mx + mn) * 0.5f;
    if (d > 1e-5f) {
        out.s = out.l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r) {
            out.h = ((g - b) / d) + (g < b ? 6.0f : 0.0f);
        } else if (mx == g) {
            out.h = ((b - r) / d) + 2.0f;
        } else {
            out.h = ((r - g) / d) + 4.0f;
        }
        out.h *= 60.0f;
    }
    return out;
}

[[nodiscard]] inline D2D1_COLOR_F hsl_to_rgb(float h, float s, float l, float a = 1.0f) noexcept {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);
    if (s <= 1e-5f) return {.r = l, .g = l, .b = l, .a = a};

    const auto hue2rgb = [](float p, float q, float t) -> float {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < (1.0f / 6.0f)) return p + ((q - p) * 6.0f * t);
        if (t < (1.0f / 2.0f)) return q;
        if (t < (2.0f / 3.0f)) return p + ((q - p) * ((2.0f / 3.0f) - t) * 6.0f);
        return p;
    };
    const float q = l < 0.5f ? (l * (1.0f + s)) : ((l + s) - (l * s));
    const float p = (2.0f * l) - q;
    const float hn = h / 360.0f;
    return {
        .r = hue2rgb(p, q, hn + (1.0f / 3.0f)),
        .g = hue2rgb(p, q, hn),
        .b = hue2rgb(p, q, hn - (1.0f / 3.0f)),
        .a = a,
    };
}

[[nodiscard]] inline float relative_luminance(D2D1_COLOR_F c) noexcept {
    const auto lin = [](float v) -> float {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return (0.2126f * lin(c.r)) + (0.7152f * lin(c.g)) + (0.0722f * lin(c.b));
}

[[nodiscard]] inline D2D1_COLOR_F contrast_fg(D2D1_COLOR_F bg, D2D1_COLOR_F dark_fg, D2D1_COLOR_F light_fg) noexcept {
    return relative_luminance(bg) > 0.36f ? dark_fg : light_fg;
}

[[nodiscard]] inline std::optional<D2D1_COLOR_F> parse_hex_color(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (!s.empty() && s.front() == '#') s.remove_prefix(1);
    if (s.size() != 6 && s.size() != 8) return std::nullopt;

    uint32_t v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;

    if (s.size() == 6) return hex(v, 1.0f);
    const uint32_t rgb = (v >> 8u) & 0xFFFFFFu;
    const float a = static_cast<float>(v & 0xFFu) / 255.0f;
    return hex(rgb, a);
}

/// Complete visual token set consumed by `Canvas` and window chrome helpers.
struct Theme {
    std::string id = "dark";
    std::string name = "Midnight Dark";
    bool dark_mode = true;
    bool follows_system_mode = false;
    bool follows_windows_accent = false;

    D2D1_COLOR_F bg{};
    D2D1_COLOR_F surface_lowest{};
    D2D1_COLOR_F surface_low{};
    D2D1_COLOR_F surface{};
    D2D1_COLOR_F surface_high{};
    D2D1_COLOR_F surface_highest{};
    D2D1_COLOR_F on_surface{};
    D2D1_COLOR_F on_surface_variant{};
    D2D1_COLOR_F outline{};
    D2D1_COLOR_F outline_variant{};
    D2D1_COLOR_F primary{};
    D2D1_COLOR_F primary_hover{};
    D2D1_COLOR_F on_primary{};
    D2D1_COLOR_F primary_container{};
    D2D1_COLOR_F on_primary_container{};
    D2D1_COLOR_F secondary_container{};
    D2D1_COLOR_F on_secondary_container{};
    D2D1_COLOR_F error{};
    D2D1_COLOR_F error_container{};
    D2D1_COLOR_F mint{};
    D2D1_COLOR_F amber{};
    D2D1_COLOR_F scrim{};

    float pill_bg_alpha = 0.80f;
    float settings_bg_alpha = 0.88f;
    float card_alpha = 0.90f;
    float shadow_alpha_scale = 1.0f;

    int pill_backdrop = DWMSBT_TRANSIENTWINDOW;
    int settings_backdrop = DWMSBT_MAINWINDOW;
    D2D1_COLOR_F pill_border{};
    bool round_corners = true;

    std::array<D2D1_COLOR_F, 8> tag_palette{};
};

/// Reads Windows Settings -> Personalize -> AppsUseLightTheme (returns true for dark mode).
[[nodiscard]] inline bool windows_uses_dark_mode() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD val = 0;
    DWORD sz = sizeof(val);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&val), &sz);
    RegCloseKey(key);
    if (st == ERROR_SUCCESS && type == REG_DWORD) return val == 0;
    return true;
}

/// Reads the user's Windows accent color (falling back to Windows blue if unavailable/greyscale).
[[nodiscard]] inline D2D1_COLOR_F windows_accent_color() noexcept {
    const auto read_dwm_dword = [](const wchar_t* name, DWORD& out) -> bool {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return false;
        }
        DWORD sz = sizeof(out);
        DWORD type = 0;
        const LSTATUS st = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&out), &sz);
        RegCloseKey(key);
        return st == ERROR_SUCCESS && type == REG_DWORD;
    };

    DWORD raw = 0;
    if (read_dwm_dword(L"AccentColor", raw)) {
        // AccentColor is stored as 0xAABBGGRR.
        const D2D1_COLOR_F c{
            .r = static_cast<float>(raw & 0xFFu) / 255.0f,
            .g = static_cast<float>((raw >> 8u) & 0xFFu) / 255.0f,
            .b = static_cast<float>((raw >> 16u) & 0xFFu) / 255.0f,
            .a = 1.0f,
        };
        if (rgb_to_hsl(c).s > 0.05f) return c;
    }
    if (read_dwm_dword(L"ColorizationColor", raw)) {
        // ColorizationColor is stored as 0xAARRGGBB.
        const D2D1_COLOR_F c = hex(raw & 0xFFFFFFu, 1.0f);
        if (rgb_to_hsl(c).s > 0.05f) return c;
    }
    return hex(0x3b82f6);
}

[[nodiscard]] inline Theme theme_dark() {
    Theme t;
    t.id = "dark";
    t.name = "Midnight Dark";
    t.dark_mode = true;
    t.bg = hex(0x141218);
    t.surface_lowest = hex(0x0f0d13);
    t.surface_low = hex(0x1d1b20);
    t.surface = hex(0x211f26);
    t.surface_high = hex(0x2b2930);
    t.surface_highest = hex(0x36343b);
    t.on_surface = hex(0xe6e1e5);
    t.on_surface_variant = hex(0xcac4d0);
    t.outline = hex(0x938f99);
    t.outline_variant = hex(0x444148);
    t.primary = hex(0xa8c7fa);
    t.primary_hover = hex(0xb8d4ff);
    t.on_primary = hex(0x062e6f);
    t.primary_container = hex(0x0842a0);
    t.on_primary_container = hex(0xd3e3fd);
    t.secondary_container = hex(0x004d67);
    t.on_secondary_container = hex(0xc2e7ff);
    t.error = hex(0xf2b8b5);
    t.error_container = hex(0x8c1d18);
    t.mint = hex(0x70d6b6);
    t.amber = hex(0xffb951);
    t.scrim = hex(0x000000, 0.5f);
    t.pill_bg_alpha = 0.80f;
    t.settings_bg_alpha = 0.88f;
    t.card_alpha = 0.90f;
    t.shadow_alpha_scale = 1.0f;
    t.pill_backdrop = DWMSBT_TRANSIENTWINDOW;
    t.settings_backdrop = DWMSBT_MAINWINDOW;
    t.pill_border = t.outline_variant;
    t.round_corners = true;
    t.tag_palette = {hex(0xffb1c8), hex(0xa8c7fa), hex(0x70d6b6), hex(0xffb951), hex(0xd0bcff), hex(0xffb4ab), hex(0x4fd8eb), hex(0xb4d273)};
    return t;
}

[[nodiscard]] inline Theme theme_light() {
    Theme t;
    t.id = "light";
    t.name = "Daylight";
    t.dark_mode = false;
    t.bg = hex(0xf4f3f8);
    t.surface_lowest = hex(0xffffff);
    t.surface_low = hex(0xefedf5);
    t.surface = hex(0xfaf9fd);
    t.surface_high = hex(0xe7e5ee);
    t.surface_highest = hex(0xdcd9e5);
    t.on_surface = hex(0x1b1b1f);
    t.on_surface_variant = hex(0x48464f);
    t.outline = hex(0x76747e);
    t.outline_variant = hex(0xc7c4d0);
    t.primary = hex(0x1b5ec9);
    t.primary_hover = hex(0x144ca6);
    t.on_primary = hex(0xffffff);
    t.primary_container = hex(0xd8e4ff);
    t.on_primary_container = hex(0x001a43);
    t.secondary_container = hex(0xc7e7ff);
    t.on_secondary_container = hex(0x001e30);
    t.error = hex(0xba1a1a);
    t.error_container = hex(0xffdad6);
    t.mint = hex(0x0d7a5f);
    t.amber = hex(0xa85d00);
    t.scrim = hex(0x000000, 0.35f);
    t.pill_bg_alpha = 0.86f;
    t.settings_bg_alpha = 0.90f;
    t.card_alpha = 0.92f;
    t.shadow_alpha_scale = 0.45f;
    t.pill_backdrop = DWMSBT_TRANSIENTWINDOW;
    t.settings_backdrop = DWMSBT_MAINWINDOW;
    t.pill_border = t.outline_variant;
    t.round_corners = true;
    t.tag_palette = {hex(0xb5295a), hex(0x1b5ec9), hex(0x0d7a5f), hex(0xa85d00), hex(0x6b38b5), hex(0xb8372c), hex(0x006a78), hex(0x4d7319)};
    return t;
}

[[nodiscard]] inline Theme theme_nord() {
    Theme t = theme_dark();
    t.id = "nord";
    t.name = "Nord";
    t.bg = hex(0x242933);
    t.surface_lowest = hex(0x1d212a);
    t.surface_low = hex(0x2a303c);
    t.surface = hex(0x2e3440);
    t.surface_high = hex(0x3b4252);
    t.surface_highest = hex(0x434c5e);
    t.on_surface = hex(0xeceff4);
    t.on_surface_variant = hex(0xd8dee9);
    t.outline = hex(0x8f9ab0);
    t.outline_variant = hex(0x4c566a);
    t.primary = hex(0x88c0d0);
    t.primary_hover = hex(0x9cd0de);
    t.on_primary = hex(0x18252d);
    t.primary_container = hex(0x35586b);
    t.on_primary_container = hex(0xd8f0f7);
    t.secondary_container = hex(0x3c526e);
    t.on_secondary_container = hex(0xd3e4f8);
    t.error = hex(0xeb8c94);
    t.error_container = hex(0x7d2932);
    t.mint = hex(0xa3be8c);
    t.amber = hex(0xebcb8b);
    t.pill_border = t.outline_variant;
    t.tag_palette = {hex(0xb48ead), hex(0x88c0d0), hex(0xa3be8c), hex(0xebcb8b), hex(0x81a1c1), hex(0xd08770), hex(0x8fbcbb), hex(0xbf616a)};
    return t;
}

[[nodiscard]] inline Theme theme_mocha() {
    Theme t = theme_dark();
    t.id = "mocha";
    t.name = "Catppuccin Mocha";
    t.bg = hex(0x181825);
    t.surface_lowest = hex(0x11111b);
    t.surface_low = hex(0x1e1e2e);
    t.surface = hex(0x242437);
    t.surface_high = hex(0x313244);
    t.surface_highest = hex(0x45475a);
    t.on_surface = hex(0xcdd6f4);
    t.on_surface_variant = hex(0xbac2de);
    t.outline = hex(0x9399b2);
    t.outline_variant = hex(0x45475a);
    t.primary = hex(0xcba6f7);
    t.primary_hover = hex(0xdac0fa);
    t.on_primary = hex(0x231338);
    t.primary_container = hex(0x4e3570);
    t.on_primary_container = hex(0xf0e2ff);
    t.secondary_container = hex(0x2c4b66);
    t.on_secondary_container = hex(0xcbe6ff);
    t.error = hex(0xf38ba8);
    t.error_container = hex(0x7d233c);
    t.mint = hex(0xa6e3a1);
    t.amber = hex(0xf9e2af);
    t.pill_border = t.outline_variant;
    t.tag_palette = {hex(0xf5c2e7), hex(0x89b4fa), hex(0xa6e3a1), hex(0xf9e2af), hex(0xcba6f7), hex(0xf38ba8), hex(0x94e2d5), hex(0xfab387)};
    return t;
}

[[nodiscard]] inline Theme theme_oled() {
    Theme t = theme_dark();
    t.id = "oled";
    t.name = "Pure OLED";
    t.bg = hex(0x000000);
    t.surface_lowest = hex(0x000000);
    t.surface_low = hex(0x0a0a0d);
    t.surface = hex(0x111115);
    t.surface_high = hex(0x1a1a20);
    t.surface_highest = hex(0x26262e);
    t.on_surface = hex(0xf4f4f6);
    t.on_surface_variant = hex(0xb8b8c4);
    t.outline = hex(0x7e7e8c);
    t.outline_variant = hex(0x2d2d36);
    t.primary = hex(0x5ce1e6);
    t.primary_hover = hex(0x82ebef);
    t.on_primary = hex(0x00282b);
    t.primary_container = hex(0x004e54);
    t.on_primary_container = hex(0xc4f8fa);
    t.secondary_container = hex(0x0b3c5d);
    t.on_secondary_container = hex(0xc4e7ff);
    t.error = hex(0xff7b7b);
    t.error_container = hex(0x6e1414);
    t.mint = hex(0x4ee2a8);
    t.amber = hex(0xffc65c);
    t.pill_bg_alpha = 0.96f;
    t.settings_bg_alpha = 0.98f;
    t.card_alpha = 1.0f;
    t.pill_backdrop = DWMSBT_NONE;
    t.settings_backdrop = DWMSBT_NONE;
    t.pill_border = t.outline_variant;
    return t;
}

/// Derives a complete Material-inspired theme from an arbitrary Windows accent color + dark/light mode.
[[nodiscard]] inline Theme theme_from_accent(D2D1_COLOR_F accent, bool dark) {
    const Hsl hsl = rgb_to_hsl(accent);
    const float h = hsl.h;
    const float sat = std::clamp(hsl.s, 0.38f, 0.88f);
    const float neu_s = std::min(0.18f, sat * 0.22f);

    Theme t = dark ? theme_dark() : theme_light();
    t.id = "accent";
    t.name = "Windows Accent";
    t.dark_mode = dark;
    t.follows_system_mode = true;
    t.follows_windows_accent = true;

    if (dark) {
        t.bg = hsl_to_rgb(h, neu_s, 0.08f);
        t.surface_lowest = hsl_to_rgb(h, neu_s, 0.06f);
        t.surface_low = hsl_to_rgb(h, neu_s, 0.11f);
        t.surface = hsl_to_rgb(h, neu_s, 0.13f);
        t.surface_high = hsl_to_rgb(h, neu_s, 0.17f);
        t.surface_highest = hsl_to_rgb(h, neu_s, 0.21f);
        t.on_surface = hsl_to_rgb(h, 0.08f, 0.91f);
        t.on_surface_variant = hsl_to_rgb(h, 0.10f, 0.78f);
        t.outline = hsl_to_rgb(h, 0.10f, 0.58f);
        t.outline_variant = hsl_to_rgb(h, 0.12f, 0.27f);

        t.primary = hsl_to_rgb(h, std::clamp(sat, 0.50f, 0.90f), 0.77f);
        t.primary_hover = hsl_to_rgb(h, std::clamp(sat, 0.50f, 0.90f), 0.84f);
        t.on_primary = contrast_fg(t.primary, hsl_to_rgb(h, 0.65f, 0.15f), hex(0xffffff));
        t.primary_container = hsl_to_rgb(h, std::clamp(sat * 0.85f, 0.35f, 0.78f), 0.28f);
        t.on_primary_container = hsl_to_rgb(h, std::clamp(sat * 0.85f, 0.40f, 0.90f), 0.90f);

        const float h2 = std::fmod(h + 28.0f, 360.0f);
        t.secondary_container = hsl_to_rgb(h2, std::clamp(sat * 0.65f, 0.28f, 0.65f), 0.24f);
        t.on_secondary_container = hsl_to_rgb(h2, 0.75f, 0.88f);
    } else {
        t.bg = hsl_to_rgb(h, neu_s, 0.96f);
        t.surface_lowest = hex(0xffffff);
        t.surface_low = hsl_to_rgb(h, neu_s, 0.94f);
        t.surface = hsl_to_rgb(h, neu_s * 0.7f, 0.98f);
        t.surface_high = hsl_to_rgb(h, neu_s, 0.90f);
        t.surface_highest = hsl_to_rgb(h, neu_s, 0.86f);
        t.on_surface = hsl_to_rgb(h, 0.12f, 0.11f);
        t.on_surface_variant = hsl_to_rgb(h, 0.10f, 0.29f);
        t.outline = hsl_to_rgb(h, 0.08f, 0.47f);
        t.outline_variant = hsl_to_rgb(h, 0.12f, 0.79f);

        t.primary = hsl_to_rgb(h, std::clamp(sat, 0.52f, 0.88f), 0.41f);
        t.primary_hover = hsl_to_rgb(h, std::clamp(sat, 0.52f, 0.88f), 0.34f);
        t.on_primary = contrast_fg(t.primary, hex(0x111111), hex(0xffffff));
        t.primary_container = hsl_to_rgb(h, std::clamp(sat * 0.85f, 0.45f, 0.88f), 0.90f);
        t.on_primary_container = hsl_to_rgb(h, std::clamp(sat * 0.85f, 0.45f, 0.88f), 0.16f);

        const float h2 = std::fmod(h + 28.0f, 360.0f);
        t.secondary_container = hsl_to_rgb(h2, 0.60f, 0.89f);
        t.on_secondary_container = hsl_to_rgb(h2, 0.65f, 0.16f);
    }
    t.pill_border = t.outline_variant;
    return t;
}

[[nodiscard]] inline Theme theme_system() {
    Theme t = windows_uses_dark_mode() ? theme_dark() : theme_light();
    t.id = "system";
    t.name = "Follow Windows";
    t.follows_system_mode = true;
    return t;
}

[[nodiscard]] inline Theme theme_accent() {
    return theme_from_accent(windows_accent_color(), windows_uses_dark_mode());
}

[[nodiscard]] inline Theme theme_liquid_glass() {
    const bool dark = windows_uses_dark_mode();
    Theme t = dark ? theme_dark() : theme_light();
    t.id = "liquid-glass";
    t.name = "Liquid Glass (Exp.)";
    t.follows_system_mode = true;
    t.pill_backdrop = DWMSBT_NONE;
    t.settings_backdrop = DWMSBT_NONE;
    if (dark) {
        t.bg = hex(0x0c1018);
        t.surface_lowest = hex(0x080b12);
        t.surface_low = hex(0x141a26);
        t.surface = hex(0x182030);
        t.surface_high = hex(0x222c40);
        t.surface_highest = hex(0x2c3850);
        t.outline_variant = hex(0x5b6b88);
        t.primary = hex(0x7dd3fc);
        t.primary_hover = hex(0xa5e1fd);
        t.on_primary = hex(0x042438);
        t.primary_container = hex(0x0c4a6e);
        t.on_primary_container = hex(0xe0f2fe);
        t.pill_bg_alpha = 0.18f;
        t.settings_bg_alpha = 0.24f;
        t.card_alpha = 0.56f;
    } else {
        t.bg = hex(0xf2f6fc);
        t.surface_lowest = hex(0xffffff);
        t.surface_low = hex(0xeef3fb);
        t.surface = hex(0xf6f9fe);
        t.surface_high = hex(0xe2eaf6);
        t.surface_highest = hex(0xd4dfef);
        t.outline_variant = hex(0x9fb0cc);
        t.primary = hex(0x0265b8);
        t.primary_hover = hex(0x025196);
        t.on_primary = hex(0xffffff);
        t.primary_container = hex(0xd1e9ff);
        t.on_primary_container = hex(0x002447);
        t.pill_bg_alpha = 0.22f;
        t.settings_bg_alpha = 0.28f;
        t.card_alpha = 0.64f;
    }
    t.pill_border = with_alpha(hex(0xffffff), dark ? 0.28f : 0.65f);
    return t;
}

[[nodiscard]] inline Theme resolve_builtin_theme(std::string_view id) {
    if (id == "system") return theme_system();
    if (id == "accent") return theme_accent();
    if (id == "light") return theme_light();
    if (id == "nord") return theme_nord();
    if (id == "mocha") return theme_mocha();
    if (id == "oled") return theme_oled();
    if (id == "liquid-glass") return theme_liquid_glass();
    return theme_dark();
}

/// String-based theme override specification (deserialized from JSON or constructed directly in tests).
struct ThemeSpec {
    std::string name;
    std::string base;                     // optional: dark | light | system | accent | nord | mocha | oled
    std::string mode;                     // optional: dark | light | system
    std::string pill_backdrop;            // optional: acrylic | mica | tabbed | none
    std::string settings_backdrop;        // optional: acrylic | mica | tabbed | none
    std::optional<float> pill_bg_alpha;
    std::optional<float> settings_bg_alpha;
    std::optional<float> card_alpha;
    std::optional<float> shadow_alpha_scale;
    std::optional<bool> round_corners;

    std::string bg;
    std::string surface_lowest;
    std::string surface_low;
    std::string surface;
    std::string surface_high;
    std::string surface_highest;
    std::string on_surface;
    std::string on_surface_variant;
    std::string outline;
    std::string outline_variant;
    std::string primary;
    std::string primary_hover;
    std::string on_primary;
    std::string primary_container;
    std::string on_primary_container;
    std::string secondary_container;
    std::string on_secondary_container;
    std::string error;
    std::string error_container;
    std::string mint;
    std::string amber;
    std::string scrim;
    std::string pill_border;
    std::vector<std::string> tag_palette;
};

[[nodiscard]] inline std::optional<int> parse_backdrop_name(std::string_view s) noexcept {
    if (s == "none" || s == "solid") return DWMSBT_NONE;
    if (s == "mica") return DWMSBT_MAINWINDOW;
    if (s == "acrylic") return DWMSBT_TRANSIENTWINDOW;
    if (s == "tabbed") return DWMSBT_TABBEDWINDOW;
    return std::nullopt;
}

/// Applies `spec` onto its `base` preset without altering any application logic.
[[nodiscard]] inline Theme apply_theme_spec(const ThemeSpec& spec, std::string id, std::string_view fallback_name) {
    std::string_view base_id = spec.base;
    if (base_id.empty()) {
        if (spec.mode == "light") {
            base_id = "light";
        } else if (spec.mode == "system") {
            base_id = "system";
        } else {
            base_id = "dark";
        }
    }
    Theme t = resolve_builtin_theme(base_id);
    t.id = std::move(id);
    t.name = !spec.name.empty() ? spec.name : std::string{fallback_name};

    if (spec.mode == "dark") {
        t.dark_mode = true;
        t.follows_system_mode = false;
    } else if (spec.mode == "light") {
        t.dark_mode = false;
        t.follows_system_mode = false;
    } else if (spec.mode == "system") {
        t.dark_mode = windows_uses_dark_mode();
        t.follows_system_mode = true;
    }

    if (auto b = parse_backdrop_name(spec.pill_backdrop)) t.pill_backdrop = *b;
    if (auto b = parse_backdrop_name(spec.settings_backdrop)) t.settings_backdrop = *b;
    if (spec.pill_bg_alpha) t.pill_bg_alpha = std::clamp(*spec.pill_bg_alpha, 0.0f, 1.0f);
    if (spec.settings_bg_alpha) t.settings_bg_alpha = std::clamp(*spec.settings_bg_alpha, 0.0f, 1.0f);
    if (spec.card_alpha) t.card_alpha = std::clamp(*spec.card_alpha, 0.0f, 1.0f);
    if (spec.shadow_alpha_scale) t.shadow_alpha_scale = std::clamp(*spec.shadow_alpha_scale, 0.0f, 2.0f);
    if (spec.round_corners) t.round_corners = *spec.round_corners;

    const auto apply_hex = [](std::string_view s, D2D1_COLOR_F& dst) -> bool {
        if (s.empty()) return false;
        if (auto c = parse_hex_color(s)) {
            dst = *c;
            return true;
        }
        return false;
    };

    apply_hex(spec.bg, t.bg);
    apply_hex(spec.surface_lowest, t.surface_lowest);
    apply_hex(spec.surface_low, t.surface_low);
    apply_hex(spec.surface, t.surface);
    apply_hex(spec.surface_high, t.surface_high);
    apply_hex(spec.surface_highest, t.surface_highest);
    apply_hex(spec.on_surface, t.on_surface);
    apply_hex(spec.on_surface_variant, t.on_surface_variant);
    apply_hex(spec.outline, t.outline);
    const bool outline_var_set = apply_hex(spec.outline_variant, t.outline_variant);
    const bool primary_set = apply_hex(spec.primary, t.primary);
    if (!apply_hex(spec.primary_hover, t.primary_hover) && primary_set) {
        t.primary_hover = lerp_color(t.primary, t.dark_mode ? hex(0xffffff) : hex(0x000000), 0.16f);
    }
    apply_hex(spec.on_primary, t.on_primary);
    apply_hex(spec.primary_container, t.primary_container);
    apply_hex(spec.on_primary_container, t.on_primary_container);
    apply_hex(spec.secondary_container, t.secondary_container);
    apply_hex(spec.on_secondary_container, t.on_secondary_container);
    apply_hex(spec.error, t.error);
    apply_hex(spec.error_container, t.error_container);
    apply_hex(spec.mint, t.mint);
    apply_hex(spec.amber, t.amber);
    apply_hex(spec.scrim, t.scrim);
    if (!apply_hex(spec.pill_border, t.pill_border) && outline_var_set) {
        t.pill_border = t.outline_variant;
    }
    if (!spec.tag_palette.empty()) {
        for (size_t i = 0; i < t.tag_palette.size(); ++i) {
            apply_hex(spec.tag_palette[i % spec.tag_palette.size()], t.tag_palette[i]);
        }
    }
    return t;
}

} // namespace usyseek::ui
