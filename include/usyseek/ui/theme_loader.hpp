#pragma once

#include "theme.hpp"

#include <usyseek/settings.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace usyseek::ui {

inline const fs::path& themes_dir() {
    static const fs::path p = data_dir() / L"themes";
    return p;
}

inline constexpr const wchar_t* EXAMPLE_THEME_FILENAME = L"example-sakura (copy but do not edit).json";
inline constexpr const wchar_t* THEME_SCHEMA_FILENAME = L"theme.schema.json";

inline std::string_view theme_schema_json() noexcept {
    return R"json({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "usyseek Theme",
  "description": "Custom color, backdrop, radius, and Liquid Glass theme specification for usyseek.",
  "type": "object",
  "additionalProperties": true,
  "$defs": {
    "hexColor": {
      "type": "string",
      "pattern": "^#?([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$",
      "description": "6-digit (#RRGGBB) or 8-digit (#RRGGBBAA) hex color."
    }
  },
  "properties": {
    "$schema": { "type": "string" },
    "name": { "type": "string", "description": "Display name shown in Settings." },
    "base": {
      "type": "string",
      "enum": ["dark", "light", "system", "accent", "nord", "mocha", "oled", "liquid-glass"],
      "description": "Built-in preset to inherit unspecified tokens from."
    },
    "mode": {
      "type": "string",
      "enum": ["dark", "light", "system"],
      "description": "Window dark/light polarity override."
    },
    "liquid_glass": {
      "type": "boolean",
      "description": "Enables the Liquid Glass refraction shader and uses the liquid-glass base defaults when base is omitted."
    },
    "pill_backdrop": {
      "type": "string",
      "enum": ["acrylic", "mica", "tabbed", "none", "solid"],
      "description": "DWM backdrop behind the search pill (ignored when liquid_glass is true or pill_radius is set)."
    },
    "settings_backdrop": {
      "type": "string",
      "enum": ["acrylic", "mica", "tabbed", "none", "solid"],
      "description": "DWM backdrop behind Settings & Theme Editor windows (ignored when liquid_glass is true)."
    },
    "pill_bg_alpha": { "type": "number", "minimum": 0.0, "maximum": 1.0 },
    "settings_bg_alpha": { "type": "number", "minimum": 0.0, "maximum": 1.0 },
    "card_alpha": { "type": "number", "minimum": 0.0, "maximum": 1.0 },
    "shadow_alpha_scale": { "type": "number", "minimum": 0.0, "maximum": 2.0 },
    "round_corners": { "type": "boolean" },
    "pill_radius": {
      "type": ["number", "null"],
      "minimum": 0.0,
      "maximum": 128.0,
      "description": "Custom search pill corner radius in DIPs (null uses the fast native DWM/default radius)."
    },
    "card_radius": { "type": "number", "minimum": 0.0, "maximum": 64.0 },
    "button_radius": { "type": "number", "minimum": 0.0, "maximum": 64.0 },
    "thumb_radius": { "type": "number", "minimum": 0.0, "maximum": 64.0 },
    "glass": {
      "type": "object",
      "additionalProperties": false,
      "description": "Optional Liquid Glass shader material parameters (active when liquid_glass is true).",
      "properties": {
        "blur": { "type": "number", "minimum": 0.0, "maximum": 6.0 },
        "roundness": { "type": "number", "minimum": 2.0, "maximum": 8.0 },
        "refraction": { "type": "number", "minimum": 1.0, "maximum": 2.0 },
        "dispersion": { "type": "number", "minimum": 0.0, "maximum": 12.0 },
        "glare_scale": { "type": "number", "minimum": 0.0, "maximum": 2.0 },
        "glare_angle": { "type": "number", "minimum": -180.0, "maximum": 180.0 }
      }
    },
    "bg": { "$ref": "#/$defs/hexColor" },
    "surface_lowest": { "$ref": "#/$defs/hexColor" },
    "surface_low": { "$ref": "#/$defs/hexColor" },
    "surface": { "$ref": "#/$defs/hexColor" },
    "surface_high": { "$ref": "#/$defs/hexColor" },
    "surface_highest": { "$ref": "#/$defs/hexColor" },
    "on_surface": { "$ref": "#/$defs/hexColor" },
    "on_surface_variant": { "$ref": "#/$defs/hexColor" },
    "outline": { "$ref": "#/$defs/hexColor" },
    "outline_variant": { "$ref": "#/$defs/hexColor" },
    "primary": { "$ref": "#/$defs/hexColor" },
    "primary_hover": { "$ref": "#/$defs/hexColor" },
    "on_primary": { "$ref": "#/$defs/hexColor" },
    "primary_container": { "$ref": "#/$defs/hexColor" },
    "on_primary_container": { "$ref": "#/$defs/hexColor" },
    "secondary_container": { "$ref": "#/$defs/hexColor" },
    "on_secondary_container": { "$ref": "#/$defs/hexColor" },
    "error": { "$ref": "#/$defs/hexColor" },
    "error_container": { "$ref": "#/$defs/hexColor" },
    "mint": { "$ref": "#/$defs/hexColor" },
    "amber": { "$ref": "#/$defs/hexColor" },
    "scrim": { "$ref": "#/$defs/hexColor" },
    "pill_border": { "$ref": "#/$defs/hexColor" },
    "tag_palette": {
      "type": "array",
      "items": { "$ref": "#/$defs/hexColor" },
      "minItems": 1
    }
  }
}
)json";
}

inline std::string_view example_theme_json() noexcept {
    return R"json({
  "$schema": "./theme.schema.json",
  "name": "Sakura (Example)",
  "base": "dark",
  "mode": "dark",
  "liquid_glass": false,
  "pill_backdrop": "acrylic",
  "settings_backdrop": "mica",
  "pill_bg_alpha": 0.82,
  "settings_bg_alpha": 0.88,
  "card_alpha": 0.90,
  "shadow_alpha_scale": 1.0,
  "round_corners": true,
  "pill_radius": null,
  "card_radius": 14.0,
  "button_radius": 10.0,
  "thumb_radius": 10.0,
  "glass": {
    "blur": 1.4,
    "roundness": 4.6,
    "refraction": 1.56,
    "dispersion": 5.5,
    "glare_scale": 1.0,
    "glare_angle": -45.0
  },
  "bg": "#1a1118",
  "surface_lowest": "#130c12",
  "surface_low": "#221720",
  "surface": "#281b25",
  "surface_high": "#33232f",
  "surface_highest": "#3f2c3a",
  "on_surface": "#f4e6ef",
  "on_surface_variant": "#d7bccd",
  "outline": "#9f8295",
  "outline_variant": "#523a4a",
  "primary": "#ff9ecf",
  "primary_hover": "#ffb8dc",
  "on_primary": "#4a1030",
  "primary_container": "#6f1d4a",
  "on_primary_container": "#ffd8eb",
  "secondary_container": "#512b49",
  "on_secondary_container": "#f7d8ef",
  "error": "#ffb4ab",
  "error_container": "#93000a",
  "mint": "#80e5c0",
  "amber": "#ffc570",
  "scrim": "#00000080",
  "pill_border": "#523a4a",
  "tag_palette": [
    "#ff9ecf",
    "#a8c7fa",
    "#80e5c0",
    "#ffc570",
    "#d8b4ff",
    "#ffb4ab",
    "#72e0eb",
    "#c3e88d"
  ]
}
)json";
}

/// Ensures `data/themes/` exists and keeps `theme.schema.json` and `example-sakura (copy but do not edit).json` synced.
inline void ensure_themes_dir() {
    const fs::path& dir = themes_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto sync_file = [&](std::wstring_view filename, std::string_view want) {
        const fs::path p = dir / filename;
        const auto cur = ulp::fs::read_as_bytes(p);
        if (!cur || *cur != want) {
            (void)ulp::fs::write(p, want);
        }
    };
    sync_file(THEME_SCHEMA_FILENAME, theme_schema_json());
    sync_file(EXAMPLE_THEME_FILENAME, example_theme_json());
}

struct ThemeEntry {
    std::string id;
    std::string name;
    std::wstring name_w;
    bool custom = false;
    bool is_example = false;
    fs::path path;
    std::optional<ThemeSpec> spec;
    D2D1_COLOR_F swatch{};
};

/// True for the built-in synced example file (`example-sakura (copy but do not edit).json`).
[[nodiscard]] inline bool is_example_theme_entry(const ThemeEntry& e) noexcept {
    return e.is_example;
}

/// Converts a display name into a safe filename stem for `data/themes/<stem>.json`.
[[nodiscard]] inline std::string sanitize_theme_stem(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool prev_dash = true;
    for (const unsigned char uc : name) {
        if (uc >= 'A' && uc <= 'Z') {
            out.push_back(static_cast<char>(uc - 'A' + 'a'));
            prev_dash = false;
        } else if ((uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') || uc == '_' || uc >= 0x80) {
            out.push_back(static_cast<char>(uc));
            prev_dash = false;
        } else if (!prev_dash) {
            out.push_back('-');
            prev_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty() || out == "theme.schema" || out == "example-sakura-(copy-but-do-not-edit)") {
        return "custom-theme";
    }
    return out;
}

/// Returns a non-colliding `.json` path in `themes_dir()` for a new theme named `name`.
[[nodiscard]] inline fs::path unique_theme_path(std::string_view name) {
    ensure_themes_dir();
    const std::string base = sanitize_theme_stem(name);
    const fs::path& dir = themes_dir();
    std::error_code ec;
    fs::path p = dir / to_wide(ulp::str::concat_strings(base, ".json"));
    for (int n = 2; fs::exists(p, ec); ++n) {
        p = dir / to_wide(ulp::str::concat_strings(base, "-", to_fixed_str(n), ".json"));
    }
    return p;
}

/// Builds a `ThemeSpec` starting from an existing `ThemeEntry` (for Duplicate / New).
[[nodiscard]] inline ThemeSpec make_duplicate_theme_spec(const ThemeEntry& src, std::string new_name) {
    ThemeSpec out;
    if (src.custom && src.spec) {
        out = *src.spec;
    } else {
        out.base = src.id.empty() ? "dark" : src.id;
        if (out.base == "liquid-glass") out.liquid_glass = true;
    }
    out.name = std::move(new_name);
    return out;
}

/// Serializes `spec` cleanly to `path` (omitting empty/unset overrides and referencing `./theme.schema.json`).
inline bool save_theme_file(const fs::path& path, const ThemeSpec& spec) {
    ensure_themes_dir();
    std::string out;
    out.reserve(1024);
    out += "{\n  \"$schema\": \"./theme.schema.json\"";
    std::string tmp_json;
    const auto append_str = [&](std::string_view key, std::string_view val) {
        if (val.empty()) return;
        tmp_json.clear();
        (void)glz::write<glz::opts{}>(val, tmp_json);
        out += ",\n  \"";
        out += key;
        out += "\": ";
        out += tmp_json;
    };
    const auto append_bool = [&](std::string_view key, std::optional<bool> val) {
        if (!val) return;
        out += ",\n  \"";
        out += key;
        out += "\": ";
        out += (*val ? "true" : "false");
    };
    const auto append_float = [&](std::string_view key, std::optional<float> val) {
        if (!val) return;
        tmp_json.clear();
        (void)glz::write<glz::opts{}>(*val, tmp_json);
        out += ",\n  \"";
        out += key;
        out += "\": ";
        out += tmp_json;
    };

    append_str("name", spec.name);
    append_str("base", spec.base);
    append_str("mode", spec.mode);
    append_bool("liquid_glass", spec.liquid_glass);
    append_str("pill_backdrop", spec.pill_backdrop);
    append_str("settings_backdrop", spec.settings_backdrop);
    append_float("pill_bg_alpha", spec.pill_bg_alpha);
    append_float("settings_bg_alpha", spec.settings_bg_alpha);
    append_float("card_alpha", spec.card_alpha);
    append_float("shadow_alpha_scale", spec.shadow_alpha_scale);
    append_bool("round_corners", spec.round_corners);
    append_float("pill_radius", spec.pill_radius);
    append_float("card_radius", spec.card_radius);
    append_float("button_radius", spec.button_radius);
    append_float("thumb_radius", spec.thumb_radius);

    if (spec.glass && (spec.glass->blur || spec.glass->roundness || spec.glass->refraction ||
                       spec.glass->dispersion || spec.glass->glare_scale || spec.glass->glare_angle)) {
        out += ",\n  \"glass\": {";
        bool first_g = true;
        const auto append_g = [&](std::string_view k, std::optional<float> v) {
            if (!v) return;
            if (!first_g) out += ',';
            first_g = false;
            tmp_json.clear();
            (void)glz::write<glz::opts{}>(*v, tmp_json);
            out += "\n    \"";
            out += k;
            out += "\": ";
            out += tmp_json;
        };
        append_g("blur", spec.glass->blur);
        append_g("roundness", spec.glass->roundness);
        append_g("refraction", spec.glass->refraction);
        append_g("dispersion", spec.glass->dispersion);
        append_g("glare_scale", spec.glass->glare_scale);
        append_g("glare_angle", spec.glass->glare_angle);
        out += "\n  }";
    }

    append_str("bg", spec.bg);
    append_str("surface_lowest", spec.surface_lowest);
    append_str("surface_low", spec.surface_low);
    append_str("surface", spec.surface);
    append_str("surface_high", spec.surface_high);
    append_str("surface_highest", spec.surface_highest);
    append_str("on_surface", spec.on_surface);
    append_str("on_surface_variant", spec.on_surface_variant);
    append_str("outline", spec.outline);
    append_str("outline_variant", spec.outline_variant);
    append_str("primary", spec.primary);
    append_str("primary_hover", spec.primary_hover);
    append_str("on_primary", spec.on_primary);
    append_str("primary_container", spec.primary_container);
    append_str("on_primary_container", spec.on_primary_container);
    append_str("secondary_container", spec.secondary_container);
    append_str("on_secondary_container", spec.on_secondary_container);
    append_str("error", spec.error);
    append_str("error_container", spec.error_container);
    append_str("mint", spec.mint);
    append_str("amber", spec.amber);
    append_str("scrim", spec.scrim);
    append_str("pill_border", spec.pill_border);

    if (!spec.tag_palette.empty()) {
        out += ",\n  \"tag_palette\": [\n";
        for (size_t i = 0; i < spec.tag_palette.size(); ++i) {
            if (i > 0) out += ",\n";
            tmp_json.clear();
            (void)glz::write<glz::opts{}>(spec.tag_palette[i], tmp_json);
            out += "    ";
            out += tmp_json;
        }
        out += "\n  ]";
    }
    out += "\n}\n";

    const fs::path tmp = ulp::str::concat_strings(path.wstring(), L".tmp.", to_fixed_wstr(GetCurrentProcessId()).view());
    if (!ulp::fs::write(tmp, std::string_view{out})) return false;
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) return true;
    std::error_code ec;
    fs::remove(tmp, ec);
    return false;
}

/// Discovers built-in and JSON themes in `data/themes/` and resolves the active `Theme`.
class ThemeCatalog {
public:
    void reload(std::vector<std::string>* errors = nullptr) {
        entries_.clear();
        static constexpr std::array<std::string_view, 8> builtin_ids = {
            "system", "accent", "dark", "light", "nord", "mocha", "oled", "liquid-glass",};
        for (const std::string_view id : builtin_ids) {
            Theme t = resolve_builtin_theme(id);
            std::wstring nw = to_wide(t.name);
            entries_.emplace_back(std::move(t.id), std::move(t.name), std::move(nw), false, false, fs::path{}, std::nullopt, t.primary);
        }

        ensure_themes_dir();
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(themes_dir(), ec)) {
            if (!e.is_regular_file(ec)) continue;
            const fs::path& ep = e.path();
            if (_wcsicmp(ep.extension().c_str(), L".json") != 0) continue;
            const std::wstring_view fname{ep.filename().native()};
            if (fname.size() >= 12 && _wcsicmp(fname.data() + fname.size() - 12, L".schema.json") == 0) continue;
            files.push_back(ep);
        }
        std::ranges::sort(files);

        for (const fs::path& p : files) {
            const auto bytes = ulp::fs::read_as_bytes(p);
            if (!bytes) {
                if (errors) errors->push_back(ulp::str::concat_strings("Could not read theme file: ", to_utf8(p.filename().native())));
                continue;
            }
            ThemeSpec spec;
            const auto err = glz::read<JSON_READ>(spec, *bytes);
            if (err) {
                if (errors) errors->push_back(ulp::str::concat_strings("Invalid JSON in theme file: ", to_utf8(p.filename().native())));
                continue;
            }
            const std::string stem = to_utf8(p.stem().native());
            std::string id = ulp::str::concat_strings("custom:", stem);
            std::string disp = spec.name.empty() ? stem : spec.name;
            std::wstring disp_w = to_wide(disp);
            const bool is_ex = _wcsicmp(p.filename().c_str(), EXAMPLE_THEME_FILENAME) == 0;
            const Theme preview = apply_theme_spec(spec, id, disp);
            entries_.emplace_back(std::move(id), std::move(disp), std::move(disp_w), true, is_ex, p, std::move(spec), preview.primary);
        }
    }

    [[nodiscard]] const std::vector<ThemeEntry>& entries() const noexcept { return entries_; }

    [[nodiscard]] const ThemeEntry* find(std::string_view id_or_name) const noexcept {
        for (const ThemeEntry& e : entries_) {
            if (e.id == id_or_name || e.name == id_or_name) return &e;
            if (e.custom && std::string_view{e.id}.starts_with("custom:") && std::string_view{e.id}.substr(7) == id_or_name) {
                return &e;
            }
        }
        return nullptr;
    }

    void refresh_dynamic_swatches() {
        for (ThemeEntry& e : entries_) {
            if (!e.custom && (e.id == "system" || e.id == "accent" || e.id == "liquid-glass")) {
                e.swatch = resolve_builtin_theme(e.id).primary;
            } else if (e.custom && e.spec) {
                e.swatch = apply_theme_spec(*e.spec, e.id, e.name).primary;
            }
        }
    }

    [[nodiscard]] Theme resolve(std::string_view id_or_name) const {
        if (id_or_name.empty() || id_or_name == "system" || id_or_name == "accent" ||
            id_or_name == "dark" || id_or_name == "light" || id_or_name == "nord" ||
            id_or_name == "mocha" || id_or_name == "oled" || id_or_name == "liquid-glass") {
            return resolve_builtin_theme(id_or_name.empty() ? "system" : id_or_name);
        }
        for (const ThemeEntry& e : entries_) {
            if (!e.custom || !e.spec) continue;
            const std::string_view stem = std::string_view{e.id}.starts_with("custom:") ? std::string_view{e.id}.substr(7) : std::string_view{};
            if (e.id == id_or_name || stem == id_or_name || e.name == id_or_name) {
                return apply_theme_spec(*e.spec, e.id, e.name);
            }
        }
        return resolve_builtin_theme("system");
    }

private:
    std::vector<ThemeEntry> entries_;
};

} // namespace usyseek::ui
