#include "check.hpp"

#include <usyseek/ui/theme.hpp>

#include <cmath>

namespace {

bool approx(float a, float b, float eps = 0.02f) {
    return std::abs(a - b) <= eps;
}

} // namespace

int main() {
    using namespace usyseek::ui;

    // 1. Hex parsing
    {
        const auto c6 = parse_hex_color(" #a8c7fa ");
        CHECK(c6.has_value());
        CHECK(approx(c6->r, 0xa8 / 255.0f, 0.005f));
        CHECK(approx(c6->g, 0xc7 / 255.0f, 0.005f));
        CHECK(approx(c6->b, 0xfa / 255.0f, 0.005f));
        CHECK(approx(c6->a, 1.0f, 0.001f));

        const auto c8 = parse_hex_color("11223380");
        CHECK(c8.has_value());
        CHECK(approx(c8->r, 0x11 / 255.0f, 0.005f));
        CHECK(approx(c8->a, 0x80 / 255.0f, 0.005f));

        CHECK(!parse_hex_color("#12345").has_value());
        CHECK(!parse_hex_color("#zzzzzz").has_value());
    }

    // 2. HSL roundtrip
    {
        const D2D1_COLOR_F orig = hex(0x3b82f6);
        const Hsl hsl = rgb_to_hsl(orig);
        const D2D1_COLOR_F back = hsl_to_rgb(hsl.h, hsl.s, hsl.l);
        CHECK(approx(orig.r, back.r, 0.01f));
        CHECK(approx(orig.g, back.g, 0.01f));
        CHECK(approx(orig.b, back.b, 0.01f));
    }

    // 3. Built-in presets contrast & polarity
    {
        const Theme d = resolve_builtin_theme("dark");
        const Theme l = resolve_builtin_theme("light");
        const Theme n = resolve_builtin_theme("nord");
        const Theme m = resolve_builtin_theme("mocha");
        const Theme o = resolve_builtin_theme("oled");

        CHECK(d.dark_mode);
        CHECK(!l.dark_mode);
        CHECK(relative_luminance(d.bg) < relative_luminance(d.on_surface));
        CHECK(relative_luminance(l.bg) > relative_luminance(l.on_surface));
        CHECK(n.id == "nord" && n.dark_mode);
        CHECK(m.id == "mocha" && m.dark_mode);
        CHECK(o.id == "oled" && o.pill_backdrop == DWMSBT_NONE);

        const Theme sys = resolve_builtin_theme("system");
        CHECK(sys.id == "system");
        CHECK(sys.follows_system_mode);

        const Theme acc = resolve_builtin_theme("accent");
        CHECK(acc.id == "accent");
        CHECK(acc.follows_system_mode);
        CHECK(acc.follows_windows_accent);
    }

    // 4. Windows accent theme derivation across dark & light modes
    {
        for (uint32_t rgb : {0xef4444u, 0x10b981u, 0x3b82f6u, 0xf59e0bu, 0xa855f7u}) {
            const Theme td = theme_from_accent(hex(rgb), true);
            CHECK(td.dark_mode);
            CHECK(relative_luminance(td.bg) < 0.08f);
            CHECK(relative_luminance(td.on_surface) > 0.65f);
            CHECK(relative_luminance(td.primary) > 0.35f);

            const Theme tl = theme_from_accent(hex(rgb), false);
            CHECK(!tl.dark_mode);
            CHECK(relative_luminance(tl.bg) > 0.75f);
            CHECK(relative_luminance(tl.on_surface) < 0.08f);
            CHECK(relative_luminance(tl.primary) < 0.45f);
        }
    }

    // 5. ThemeSpec partial overrides + auto-derived hover/border + short tag palette cycling
    {
        ThemeSpec spec;
        spec.name = "Rose Light";
        spec.mode = "light";
        spec.pill_backdrop = "mica";
        spec.settings_backdrop = "tabbed";
        spec.primary = "#e11d48";
        spec.outline_variant = "#d4a5b2";
        spec.tag_palette = {"#e11d48", "#2563eb"};

        const Theme t = apply_theme_spec(spec, "custom:rose", "rose");
        CHECK(t.id == "custom:rose");
        CHECK(t.name == "Rose Light");
        CHECK(!t.dark_mode);
        CHECK(t.pill_backdrop == DWMSBT_MAINWINDOW);
        CHECK(t.settings_backdrop == DWMSBT_TABBEDWINDOW);
        CHECK(approx(t.primary.r, 0xe1 / 255.0f, 0.005f));
        // Auto-derived primary_hover should differ from primary
        CHECK(t.primary_hover.r != t.primary.r || t.primary_hover.g != t.primary.g);
        // pill_border inherits outline_variant override
        CHECK(approx(t.pill_border.r, 0xd4 / 255.0f, 0.005f));
        // Short tag_palette cycles cleanly
        CHECK(approx(t.tag_palette[0].r, 0xe1 / 255.0f, 0.005f));
        CHECK(approx(t.tag_palette[1].b, 0xeb / 255.0f, 0.005f));
        CHECK(approx(t.tag_palette[2].r, 0xe1 / 255.0f, 0.005f));
        CHECK(!t.is_liquid_glass);
    }

    // 6. Custom theme enabling liquid_glass uses liquid-glass base
    {
        ThemeSpec spec;
        spec.name = "Tint Glass";
        spec.liquid_glass = true;
        spec.mode = "light";
        spec.primary = "#e11d48";

        const Theme lg_light = theme_liquid_glass(false);
        const Theme t = apply_theme_spec(spec, "custom:tint-glass", "tint-glass");
        CHECK(t.is_liquid_glass);
        CHECK(!t.dark_mode);
        CHECK(!t.follows_system_mode);
        CHECK(t.pill_backdrop == DWMSBT_NONE);
        CHECK(t.settings_backdrop == DWMSBT_NONE);
        CHECK(approx(t.pill_bg_alpha, lg_light.pill_bg_alpha, 0.001f));
        CHECK(approx(t.settings_bg_alpha, lg_light.settings_bg_alpha, 0.001f));
        CHECK(approx(t.card_alpha, lg_light.card_alpha, 0.001f));
        CHECK(approx(t.bg.r, lg_light.bg.r, 0.005f));
        CHECK(approx(t.primary.r, 0xe1 / 255.0f, 0.005f));
    }

    // 7. Custom theme with liquid_glass without mode follows system mode & liquid-glass base
    {
        ThemeSpec spec;
        spec.name = "Auto Glass";
        spec.liquid_glass = true;

        const Theme lg = theme_liquid_glass();
        const Theme t = apply_theme_spec(spec, "custom:auto-glass", "auto-glass");
        CHECK(t.is_liquid_glass);
        CHECK(t.follows_system_mode);
        CHECK(t.dark_mode == lg.dark_mode);
        CHECK(t.pill_backdrop == DWMSBT_NONE);
        CHECK(approx(t.pill_bg_alpha, lg.pill_bg_alpha, 0.001f));
    }

    // 8. Custom theme with explicit base + liquid_glass retains base palette and gets glass material defaults
    {
        ThemeSpec spec;
        spec.name = "Nord Glass";
        spec.base = "nord";
        spec.liquid_glass = true;

        const Theme nord = theme_nord();
        const Theme lg_dark = theme_liquid_glass(true);
        const Theme t = apply_theme_spec(spec, "custom:nord-glass", "nord-glass");
        CHECK(t.is_liquid_glass);
        CHECK(t.dark_mode);
        CHECK(t.pill_backdrop == DWMSBT_NONE);
        CHECK(t.settings_backdrop == DWMSBT_NONE);
        CHECK(approx(t.pill_bg_alpha, lg_dark.pill_bg_alpha, 0.001f));
        CHECK(approx(t.bg.r, nord.bg.r, 0.005f));
        CHECK(approx(t.primary.g, nord.primary.g, 0.005f));
    }

    // 9. Explicit liquid_glass = false overrides base = "liquid-glass"
    {
        ThemeSpec spec;
        spec.base = "liquid-glass";
        spec.mode = "dark";
        spec.liquid_glass = false;

        const Theme t = apply_theme_spec(spec, "custom:flat-lg", "flat-lg");
        CHECK(!t.is_liquid_glass);
        CHECK(t.dark_mode);
    }

    return 0;
}
