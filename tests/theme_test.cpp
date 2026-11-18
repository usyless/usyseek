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
        CHECK(!t.pill_radius.has_value());
        CHECK(approx(t.card_radius, 14.0f, 0.001f));
        CHECK(approx(t.button_radius, 10.0f, 0.001f));
        CHECK(approx(t.thumb_radius, 10.0f, 0.001f));
    }

    // 10. Corner radius overrides & clamping + offset_radius helper
    {
        ThemeSpec spec;
        spec.name = "Rounded";
        spec.pill_radius = 22.0f;
        spec.card_radius = 18.0f;
        spec.button_radius = 4.0f;
        spec.thumb_radius = 0.0f;

        const Theme t = apply_theme_spec(spec, "custom:rounded", "rounded");
        CHECK(t.pill_radius.has_value() && approx(*t.pill_radius, 22.0f, 0.001f));
        CHECK(approx(t.card_radius, 18.0f, 0.001f));
        CHECK(approx(t.button_radius, 4.0f, 0.001f));
        CHECK(approx(t.thumb_radius, 0.0f, 0.001f));

        ThemeSpec clamped;
        clamped.pill_radius = 999.0f;
        clamped.card_radius = -5.0f;
        const Theme tc = apply_theme_spec(clamped, "custom:clamped", "clamped");
        CHECK(tc.pill_radius.has_value() && approx(*tc.pill_radius, 128.0f, 0.001f));
        CHECK(approx(tc.card_radius, 0.0f, 0.001f));

        CHECK(approx(offset_radius(0.0f, 2.0f), 0.0f, 0.001f));
        CHECK(approx(offset_radius(14.0f, -2.0f), 12.0f, 0.001f));
        CHECK(approx(offset_radius(1.0f, -5.0f), 0.0f, 0.001f));
    }

    // 11. Hex formatting roundtrip (opaque and translucent) + stack helpers
    {
        CHECK(format_hex_color(hex(0xa8c7fa)) == "#a8c7fa");
        CHECK(format_hex_color(hex(0xa8c7fa), true) == "#a8c7faff");
        CHECK(format_hex_color(hex(0x112233, 0.50196f)) == "#11223380");

        std::array<char, 10> nbuf{};
        CHECK(format_hex_color_into(hex(0xa8c7fa), nbuf) == "#a8c7fa");
        std::array<wchar_t, 10> wbuf{};
        CHECK(format_hex_color_w_into(hex(0xa8c7fa), wbuf) == L"#a8c7fa");
        CHECK(same_hex_color(hex(0xa8c7fa), *parse_hex_color("#a8c7fa")));
        CHECK(!same_hex_color(hex(0xa8c7fa), hex(0xa8c7fb)));
        CHECK(!same_hex_color(hex(0xa8c7fa, 1.0f), hex(0xa8c7fa, 0.5f), true));
    }

    // 12. Liquid Glass material overrides & clamping
    {
        ThemeSpec def;
        def.liquid_glass = true;
        const Theme td = apply_theme_spec(def, "custom:lg-def", "lg-def");
        CHECK(approx(td.glass.blur, 1.4f, 0.001f));
        CHECK(approx(td.glass.roundness, 4.6f, 0.001f));
        CHECK(approx(td.glass.refraction, 1.56f, 0.001f));
        CHECK(approx(td.glass.dispersion, 5.5f, 0.001f));
        CHECK(approx(td.glass.glare_scale, 1.0f, 0.001f));
        CHECK(approx(td.glass.glare_angle, -45.0f, 0.001f));

        ThemeSpec custom;
        custom.liquid_glass = true;
        custom.glass = LiquidGlassSpec{
            .blur = 2.5f,
            .roundness = 3.2f,
            .refraction = 1.75f,
            .dispersion = 8.0f,
            .glare_scale = 0.6f,
            .glare_angle = 30.0f,
        };
        const Theme tc = apply_theme_spec(custom, "custom:lg-custom", "lg-custom");
        CHECK(approx(tc.glass.blur, 2.5f, 0.001f));
        CHECK(approx(tc.glass.roundness, 3.2f, 0.001f));
        CHECK(approx(tc.glass.refraction, 1.75f, 0.001f));
        CHECK(approx(tc.glass.dispersion, 8.0f, 0.001f));
        CHECK(approx(tc.glass.glare_scale, 0.6f, 0.001f));
        CHECK(approx(tc.glass.glare_angle, 30.0f, 0.001f));

        ThemeSpec clamped;
        clamped.liquid_glass = true;
        clamped.glass = LiquidGlassSpec{
            .blur = 99.0f,
            .roundness = 0.5f,
            .refraction = 5.0f,
            .dispersion = -2.0f,
            .glare_scale = 9.0f,
            .glare_angle = 720.0f,
        };
        const Theme tclamp = apply_theme_spec(clamped, "custom:lg-clamp", "lg-clamp");
        CHECK(approx(tclamp.glass.blur, 6.0f, 0.001f));
        CHECK(approx(tclamp.glass.roundness, 2.0f, 0.001f));
        CHECK(approx(tclamp.glass.refraction, 2.0f, 0.001f));
        CHECK(approx(tclamp.glass.dispersion, 0.0f, 0.001f));
        CHECK(approx(tclamp.glass.glare_scale, 2.0f, 0.001f));
        CHECK(approx(tclamp.glass.glare_angle, 180.0f, 0.001f));
    }

    return 0;
}
