#pragma once

#include "gfx.hpp"

#include <usyseek/svg.hpp>
#include <usylibpp/ui/thumbs.hpp>

namespace usyseek::ui {

using usylibpp::ui::Raster;
using usylibpp::ui::decode_image;

/// `usylibpp::ui::Thumbs` with resvg SVG rasterization wired in.
class Thumbs : public usylibpp::ui::Thumbs {
public:
    explicit Thumbs(size_t max_bitmaps = 240, bool full = false) : usylibpp::ui::Thumbs(max_bitmaps, full) {
        set_custom_decoder([](const std::wstring& path, UINT px, Raster& out) -> bool {
            if (!svg::is_svg(path)) return false;
            const auto raster = svg::render(path, px, px, svg::Fit::Contain);
            if (!raster) return true;
            const std::span<const uint8_t> src = raster->rgba();
            out.pixels.resize(src.size());
            // Opaque RGBA -> BGRA
            for (size_t i = 0; i + 3 < src.size(); i += 4) {
                out.pixels[i] = src[i + 2];
                out.pixels[i + 1] = src[i + 1];
                out.pixels[i + 2] = src[i];
                out.pixels[i + 3] = src[i + 3];
            }
            out.w = raster->width;
            out.h = raster->height;
            return true;
        });
    }
};

} // namespace usyseek::ui
