#pragma once

#include "gfx.hpp"

#include <array>
#include <cmath>

namespace usyseek::ui {

inline D2D1_COLOR_F hex(uint32_t rgb, float a = 1.0f) noexcept {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
inline D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) noexcept {
    c.a = a;
    return c;
}

// Material 3 dark tokens (from the mockup)
namespace color {
inline const D2D1_COLOR_F bg = hex(0x141218);
inline const D2D1_COLOR_F surface_lowest = hex(0x0f0d13);
inline const D2D1_COLOR_F surface_low = hex(0x1d1b20);
inline const D2D1_COLOR_F surface = hex(0x211f26);
inline const D2D1_COLOR_F surface_high = hex(0x2b2930);
inline const D2D1_COLOR_F surface_highest = hex(0x36343b);
inline const D2D1_COLOR_F on_surface = hex(0xe6e1e5);
inline const D2D1_COLOR_F on_surface_variant = hex(0xcac4d0);
inline const D2D1_COLOR_F outline = hex(0x938f99);
inline const D2D1_COLOR_F outline_variant = hex(0x444148);
inline const D2D1_COLOR_F primary = hex(0xa8c7fa);
inline const D2D1_COLOR_F on_primary = hex(0x062e6f);
inline const D2D1_COLOR_F primary_container = hex(0x0842a0);
inline const D2D1_COLOR_F on_primary_container = hex(0xd3e3fd);
inline const D2D1_COLOR_F secondary_container = hex(0x004d67);
inline const D2D1_COLOR_F on_secondary_container = hex(0xc2e7ff);
inline const D2D1_COLOR_F error = hex(0xf2b8b5);
inline const D2D1_COLOR_F error_container = hex(0x8c1d18);
inline const D2D1_COLOR_F mint = hex(0x70d6b6);
inline const D2D1_COLOR_F amber = hex(0xffb951);
inline const D2D1_COLOR_F scrim = hex(0x000000, 0.5f);
} // namespace color

/// Stable per-tag accent colour.
inline D2D1_COLOR_F tag_color(std::string_view tag) noexcept {
    static constexpr uint32_t palette[] = {0xffb1c8, 0xa8c7fa, 0x70d6b6, 0xffb951, 0xd0bcff, 0xffb4ab, 0x4fd8eb, 0xb4d273};
    return hex(palette[rapidhash(tag.data(), tag.size()) % std::size(palette)]);
}

namespace glyph {
inline constexpr wchar_t search = 0xE721;
inline constexpr wchar_t settings = 0xE713;
inline constexpr wchar_t fullscreen = 0xE740;
inline constexpr wchar_t exit_fullscreen = 0xE73F;
inline constexpr wchar_t close = 0xE8BB;
inline constexpr wchar_t tag = 0xE8EC;
inline constexpr wchar_t similar = 0xE71E;
inline constexpr wchar_t play = 0xF5B0;
inline constexpr wchar_t pause = 0xF8AE;
inline constexpr wchar_t folder = 0xE8B7;
inline constexpr wchar_t open = 0xE8A7;
inline constexpr wchar_t copy = 0xE8C8;
inline constexpr wchar_t add = 0xE710;
inline constexpr wchar_t remove = 0xE711;
inline constexpr wchar_t lock = 0xE72E;
inline constexpr wchar_t check = 0xE73E;
inline constexpr wchar_t photo = 0xEB9F;
inline constexpr wchar_t video = 0xE714;
inline constexpr wchar_t clock = 0xE823;
inline constexpr wchar_t terminal = 0xE756;
inline constexpr wchar_t refresh = 0xE72C;
inline constexpr wchar_t del = 0xE74D;
inline constexpr wchar_t keyboard = 0xE765;
inline constexpr wchar_t chip = 0xE950;
inline constexpr wchar_t code = 0xE943;
inline constexpr wchar_t warning = 0xE7BA;
inline constexpr wchar_t download = 0xE896;
inline constexpr wchar_t block = 0xF140;
inline constexpr wchar_t chevron_down = 0xE70D;
} // namespace glyph

enum class Font : uint8_t {
    Body,       // 13
    BodySemi,
    BodyWrap,
    Large,      // 15 semibold
    Input,      // 17
    Small,      // 12
    SmallSemi,
    SmallWrap,
    Caption,    // 11
    Label,      // 11 bold, letter-spaced caps
    Mono,       // 12
    MonoSmall,  // 11
    Title,      // 17 semibold
    Hero,       // 32 semibold
    Icon,       // 16
    IconSmall,  // 12
    IconLarge,  // 20
    Count
};

class Fonts {
public:
    bool init(IDWriteFactory3* dw) {
        dw_ = dw;
        const std::wstring text = pick(L"Segoe UI Variable Text", L"Segoe UI");
        const std::wstring disp = pick(L"Segoe UI Variable Display", L"Segoe UI");
        const std::wstring mono = pick(L"Cascadia Mono", L"Consolas");
        const std::wstring icon = pick(L"Segoe Fluent Icons", L"Segoe MDL2 Assets");

        struct Def {
            Font f;
            const std::wstring* family;
            float size;
            DWRITE_FONT_WEIGHT weight;
            bool wrap;
        };
        const Def defs[] = {
            {Font::Body, &text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::BodySemi, &text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
            {Font::BodyWrap, &text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, true},
            {Font::Large, &disp, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
            {Font::Input, &text, 17.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::Small, &text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::SmallSemi, &text, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
            {Font::SmallWrap, &text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, true},
            {Font::Caption, &text, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::Label, &text, 11.0f, DWRITE_FONT_WEIGHT_BOLD, false},
            {Font::Mono, &mono, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::MonoSmall, &mono, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::Title, &disp, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
            {Font::Hero, &disp, 32.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
            {Font::Icon, &icon, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::IconSmall, &icon, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
            {Font::IconLarge, &icon, 20.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
        };
        for (const auto& d : defs) {
            wil::com_ptr<IDWriteTextFormat> fmt;
            if (FAILED(dw->CreateTextFormat(d.family->c_str(), nullptr, d.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, d.size, L"en-us", &fmt))) return false;
            fmt->SetWordWrapping(d.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            fmt->SetParagraphAlignment(d.wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (!d.wrap) {
                wil::com_ptr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.get(), &ellipsis))) {
                    const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    fmt->SetTrimming(&trim, ellipsis.get());
                }
            }
            formats_[static_cast<size_t>(d.f)] = std::move(fmt);
        }
        return true;
    }

    [[nodiscard]] IDWriteTextFormat* get(Font f) const noexcept { return formats_[static_cast<size_t>(f)].get(); }
    [[nodiscard]] IDWriteFactory3* dwrite() const noexcept { return dw_; }

    float measure(std::wstring_view s, Font f) {
        if (s.empty()) return 0.0f;
        const uint64_t key = rapidhash(s.data(), s.size() * sizeof(wchar_t)) ^ (static_cast<uint64_t>(f) * 0x9e3779b97f4a7c15ull);
        if (const auto it = width_cache_.find(key); it != width_cache_.end()) return it->second;
        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(dw_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), get(f), 100000.0f, 100.0f, &layout))) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        if (width_cache_.size() > 2048) width_cache_.clear();
        width_cache_.emplace(key, m.widthIncludingTrailingWhitespace);
        return m.widthIncludingTrailingWhitespace;
    }

private:
    std::wstring pick(const wchar_t* preferred, const wchar_t* fallback) {
        wil::com_ptr<IDWriteFontCollection> sys;
        if (SUCCEEDED(dw_->GetSystemFontCollection(&sys))) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(preferred, &idx, &exists)) && exists) return preferred;
        }
        return fallback;
    }

    IDWriteFactory3* dw_ = nullptr;
    std::array<wil::com_ptr<IDWriteTextFormat>, static_cast<size_t>(Font::Count)> formats_;
    std::unordered_map<uint64_t, float> width_cache_;
};

struct Hit {
    D2D1_RECT_F r;
    int id;
    int index;
};

inline bool contains(const D2D1_RECT_F& r, D2D1_POINT_2F p) noexcept {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}
inline D2D1_RECT_F rect(float x, float y, float w, float h) noexcept { return D2D1::RectF(x, y, x + w, y + h); }
inline D2D1_RECT_F inset(D2D1_RECT_F r, float dx, float dy) noexcept { return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy); }
inline float width(const D2D1_RECT_F& r) noexcept { return r.right - r.left; }
inline float height(const D2D1_RECT_F& r) noexcept { return r.bottom - r.top; }
inline D2D1_RECT_F snap(D2D1_RECT_F r) noexcept { return D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom)); }

enum class Align { Left, Center, Right };

/// Immediate-mode drawing helpers over one device context, plus hit-region recording.
class Canvas {
public:
    Canvas(ID2D1DeviceContext6* dc, Fonts& fonts, std::vector<Hit>& hits) : dc_{dc}, fonts_{fonts}, hits_{hits} {
        dc_->CreateSolidColorBrush(color::on_surface, &brush_);
    }

    [[nodiscard]] ID2D1DeviceContext6* dc() const noexcept { return dc_; }
    [[nodiscard]] Fonts& fonts() const noexcept { return fonts_; }

    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) {
        brush_->SetColor(c);
        return brush_.get();
    }

    void hit(D2D1_RECT_F r, int id, int index = 0) { hits_.push_back({r, id, index}); }

    void fill(D2D1_RECT_F r, D2D1_COLOR_F c) { dc_->FillRectangle(r, brush(c)); }
    void fill_round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) { dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush(c)); }
    void stroke_round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float w = 1.0f) {
        const float h = w * 0.5f;
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset(r, h, h), radius, radius), brush(c), w);
    }
    void fill_circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col) { dc_->FillEllipse(D2D1::Ellipse(c, r, r), brush(col)); }
    void stroke_circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col, float w = 1.0f) { dc_->DrawEllipse(D2D1::Ellipse(c, r, r), brush(col), w); }
    void hline(float x0, float x1, float y, D2D1_COLOR_F c) { fill(D2D1::RectF(x0, y, x1, y + 1.0f), c); }
    void vline(float x, float y0, float y1, D2D1_COLOR_F c) { fill(D2D1::RectF(x, y0, x + 1.0f, y1), c); }

    void text(std::wstring_view s, D2D1_RECT_F r, Font f, D2D1_COLOR_F c, Align a = Align::Left) {
        if (s.empty()) return;
        IDWriteTextFormat* fmt = fonts_.get(f);
        fmt->SetTextAlignment(a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
        dc_->DrawText(s.data(), static_cast<UINT32>(s.size()), fmt, r, brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void icon(wchar_t g, D2D1_RECT_F r, D2D1_COLOR_F c, Font f = Font::Icon) {
        const wchar_t s[2] = {g, 0};
        text(std::wstring_view{s, 1}, r, f, c, Align::Center);
    }

    float measure(std::wstring_view s, Font f) {
        return fonts_.measure(s, f);
    }

    void fill_bitmap_round(ID2D1Bitmap1* bmp, D2D1_RECT_F tr, float radius) {
        const D2D1_SIZE_F bs = bmp->GetSize();
        if (bs.width <= 0.0f || bs.height <= 0.0f) return;
        const float s = std::max(width(tr) / bs.width, height(tr) / bs.height);
        const float ox = tr.left + (width(tr) - bs.width * s) * 0.5f;
        const float oy = tr.top + (height(tr) - bs.height * s) * 0.5f;
        if (!bmp_brush_) {
            const auto bp = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_LINEAR);
            dc_->CreateBitmapBrush(bmp, &bp, nullptr, &bmp_brush_);
        } else {
            bmp_brush_->SetBitmap(bmp);
        }
        if (bmp_brush_) {
            bmp_brush_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(ox, oy));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(tr, radius, radius), bmp_brush_.get());
        }
    }

    /// Height of wrapped text in `w` DIPs.
    float measure_height(std::wstring_view s, Font f, float w) {
        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(fonts_.dwrite()->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), fonts_.get(f), w, 10000.0f, &layout))) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.height;
    }

    void push_clip(D2D1_RECT_F r) { dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
    void pop_clip() { dc_->PopAxisAlignedClip(); }

    /// Soft drop shadow approximated with stacked translucent rounded rects (cheap, no effects).
    void shadow(D2D1_RECT_F r, float radius, float spread = 10.0f, float alpha = 0.28f) {
        constexpr int steps = 5;
        for (int i = steps; i >= 1; --i) {
            const float t = static_cast<float>(i) / steps;
            const float g = spread * t;
            fill_round(D2D1::RectF(r.left - g * 0.6f, r.top - g * 0.2f, r.right + g * 0.6f, r.bottom + g), radius + g, hex(0x000000, alpha / steps * (1.2f - t)));
        }
    }

private:
    ID2D1DeviceContext6* dc_;
    Fonts& fonts_;
    std::vector<Hit>& hits_;
    wil::com_ptr<ID2D1SolidColorBrush> brush_;
    wil::com_ptr<ID2D1BitmapBrush1> bmp_brush_;
};

inline const Hit* hit_test(const std::vector<Hit>& hits, D2D1_POINT_2F p) {
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        if (contains(it->r, p)) return &*it;
    }
    return nullptr;
}

} // namespace usyseek::ui
