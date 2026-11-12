#pragma once

#include "common.hpp"
#include "rust_bridge.hpp"

extern "C" {
struct UsySvgInfo {
    uint32_t width;
    uint32_t height;
    uint32_t src_width;
    uint32_t src_height;
};

int32_t usy_svg_render(UsyStr path, uint32_t box_w, uint32_t box_h, uint32_t fit, uint32_t background, UsySvgInfo* info, UsyBuf* out);
}

namespace usyseek::svg {

/// How the document is mapped onto the requested box.
enum class Fit : uint8_t {
    Stretch = 0, // exactly the box, aspect ignored
    Cover = 1,   // fills the box, centre-cropped
    Contain = 2, // aspect preserved, bitmap no larger than the box
};

/// SVGs are drawn for a page, so they are rasterised onto white (0xAARRGGBB).
inline constexpr uint32_t WHITE = 0xFFFFFFFFu;

inline bool is_svg(std::wstring_view path) {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    const std::wstring ext = lower(path.substr(dot + 1));
    return ext == L"svg" || ext == L"svgz";
}

struct Raster {
    uint32_t width = 0;      // pixels in `rgba()`
    uint32_t height = 0;
    uint32_t src_width = 0;  // intrinsic size of the document
    uint32_t src_height = 0;
    RustBuf pixels;

    /// Premultiplied RGBA8, `width * height * 4` bytes.
    [[nodiscard]] std::span<const uint8_t> rgba() const noexcept {
        return {reinterpret_cast<const uint8_t*>(pixels.b.ptr), pixels.b.len};
    }
};

/// Rasterises an SVG / SVGZ file with resvg. Safe to call from any thread.
inline std::optional<Raster> render(std::wstring_view path, uint32_t box_w, uint32_t box_h, Fit fit,
                                    uint32_t background = WHITE, std::string* error = nullptr) {
    const std::string utf8 = to_utf8(path);
    Raster r;
    UsySvgInfo info{};
    if (usy_svg_render(ustr(utf8), box_w, box_h, static_cast<uint32_t>(fit), background, &info, &r.pixels.b) != 0) {
        if (error) *error = r.pixels.str();
        return std::nullopt;
    }
    if (r.pixels.b.len != static_cast<size_t>(info.width) * info.height * 4) return std::nullopt;
    r.width = info.width;
    r.height = info.height;
    r.src_width = info.src_width;
    r.src_height = info.src_height;
    return r;
}

} // namespace usyseek::svg
