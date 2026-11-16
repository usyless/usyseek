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

inline fs::path themes_dir() {
    return data_dir() / L"themes";
}

inline std::string_view example_theme_json() noexcept {
    return R"json({
  "name": "Sakura (Example)",
  "base": "dark",
  "mode": "dark",
  "pill_backdrop": "acrylic",
  "settings_backdrop": "mica",
  "pill_bg_alpha": 0.82,
  "settings_bg_alpha": 0.88,
  "card_alpha": 0.90,
  "round_corners": true,
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

/// Ensures `data/themes/` exists and seeds `example-sakura.json` if no JSON themes exist yet.
inline void ensure_themes_dir() {
    const fs::path dir = themes_dir();
    std::error_code ec;
    const bool existed = fs::exists(dir, ec);
    fs::create_directories(dir, ec);
    bool has_json = false;
    if (existed) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && lower(entry.path().extension().wstring()) == L".json") {
                has_json = true;
                break;
            }
        }
    }
    if (!has_json) {
        const fs::path ex = dir / L"example-sakura.json";
        if (!fs::exists(ex, ec)) {
            (void)ulp::fs::write(ex, example_theme_json());
        }
    }
}

struct ThemeEntry {
    std::string id;
    std::string name;
    bool custom = false;
    fs::path path;
    std::optional<ThemeSpec> spec;
    D2D1_COLOR_F swatch{};
};

/// Discovers built-in and JSON themes in `data/themes/` and resolves the active `Theme`.
class ThemeCatalog {
public:
    void reload(std::vector<std::string>* errors = nullptr) {
        entries_.clear();
        for (const char* id : {"system", "accent", "dark", "light", "nord", "mocha", "oled", "liquid-glass"}) {
            const Theme t = resolve_builtin_theme(id);
            entries_.push_back({.id = t.id, .name = t.name, .custom = false, .path = {}, .spec = std::nullopt, .swatch = t.primary});
        }

        ensure_themes_dir();
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(themes_dir(), ec)) {
            if (e.is_regular_file(ec) && lower(e.path().extension().wstring()) == L".json") {
                files.push_back(e.path());
            }
        }
        std::ranges::sort(files);

        for (const fs::path& p : files) {
            const auto bytes = ulp::fs::read_as_bytes(p);
            if (!bytes) {
                if (errors) errors->push_back("Could not read theme file: " + to_utf8(p.filename().wstring()));
                continue;
            }
            ThemeSpec spec;
            const auto err = glz::read<JSON_READ>(spec, *bytes);
            if (err) {
                if (errors) errors->push_back("Invalid JSON in theme file: " + to_utf8(p.filename().wstring()));
                continue;
            }
            const std::string stem = to_utf8(p.stem().wstring());
            const std::string id = "custom:" + stem;
            const std::string disp = spec.name.empty() ? stem : spec.name;
            const Theme preview = apply_theme_spec(spec, id, disp);
            entries_.push_back({.id = id, .name = disp, .custom = true, .path = p, .spec = std::move(spec), .swatch = preview.primary});
        }
    }

    [[nodiscard]] const std::vector<ThemeEntry>& entries() const noexcept { return entries_; }

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
            const std::string stem = to_utf8(e.path.stem().wstring());
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
