#pragma once

#include "anim.hpp"
#include "fit.hpp"
#include "gfx.hpp"

#include <algorithm>
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
[[nodiscard]] inline D2D1_COLOR_F lerp_color(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) noexcept {
    const float k = std::clamp(t, 0.0f, 1.0f);
    return {lerp(a.r, b.r, k), lerp(a.g, b.g, k), lerp(a.b, b.b, k), lerp(a.a, b.a, k)};
}
[[nodiscard]] inline D2D1_RECT_F lerp_rect(D2D1_RECT_F a, D2D1_RECT_F b, float t) noexcept {
    return {lerp(a.left, b.left, t), lerp(a.top, b.top, t), lerp(a.right, b.right, t), lerp(a.bottom, b.bottom, t)};
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
inline constexpr wchar_t help = 0xE897;
inline constexpr wchar_t zoom = 0xE8A3;
inline constexpr wchar_t link = 0xE71B;
inline constexpr wchar_t chevron_left = 0xE76B;
inline constexpr wchar_t chevron_right = 0xE76C;
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

enum class Align : uint8_t { Left, Center, Right };

/// Immediate-mode drawing helpers over one device context, plus hit-region recording.
class Canvas {
public:
    Canvas(ID2D1DeviceContext6* dc, Fonts& fonts, std::vector<Hit>& hits, float opacity = 1.0f)
        : dc_{dc}, fonts_{fonts}, hits_{hits}, opacity_{std::clamp(opacity, 0.0f, 1.0f)} {
        dc_->CreateSolidColorBrush(color::on_surface, &brush_);
    }

    [[nodiscard]] ID2D1DeviceContext6* dc() const noexcept { return dc_; }
    [[nodiscard]] Fonts& fonts() const noexcept { return fonts_; }

    struct ScopedOpacity {
        Canvas& cv;
        float prev;
        ScopedOpacity(Canvas& c, float factor) noexcept : cv{c}, prev{c.opacity_} {
            cv.opacity_ *= std::clamp(factor, 0.0f, 1.0f);
        }
        ~ScopedOpacity() { cv.opacity_ = prev; }
        ScopedOpacity(const ScopedOpacity&) = delete;
        ScopedOpacity& operator=(const ScopedOpacity&) = delete;
        ScopedOpacity(ScopedOpacity&&) = delete;
        ScopedOpacity& operator=(ScopedOpacity&&) = delete;
    };

    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) {
        c.a *= opacity_;
        brush_->SetColor(c);
        return brush_.get();
    }

    void hit(D2D1_RECT_F r, int id, int index = 0) {
        if (!clips_.empty()) {
            const D2D1_RECT_F& c = clips_.back();
            r.left = std::max(r.left, c.left);
            r.top = std::max(r.top, c.top);
            r.right = std::min(r.right, c.right);
            r.bottom = std::min(r.bottom, c.bottom);
        }
        if (r.left < r.right && r.top < r.bottom) {
            hits_.push_back({r, id, index});
        }
    }

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
        fmt->SetTextAlignment(text_alignment(a));
        dc_->DrawText(s.data(), static_cast<UINT32>(s.size()), fmt, r, brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void text_wrap(std::wstring_view s, D2D1_RECT_F r, Font f, D2D1_COLOR_F c, Align a = Align::Left) {
        if (s.empty()) return;
        wil::com_ptr<IDWriteTextLayout> layout = wrapped_layout(s, f, width(r));
        if (!layout) return;
        layout->SetTextAlignment(text_alignment(a));
        dc_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void icon(wchar_t g, D2D1_RECT_F r, D2D1_COLOR_F c, Font f = Font::Icon) {
        const wchar_t s[2] = {g, 0};
        text(std::wstring_view{s, 1}, r, f, c, Align::Center);
    }

    float measure(std::wstring_view s, Font f) {
        return fonts_.measure(s, f);
    }

    void fill_bitmap_round(ID2D1Bitmap1* bmp, D2D1_RECT_F tr, float radius, ThumbFit fit = ThumbFit::Crop) {
        const D2D1_SIZE_F bs = bmp->GetSize();
        if (bs.width <= 0.0f || bs.height <= 0.0f) return;
        const FitRect dest = fit_image(bs.width, bs.height, FitRect{tr.left, tr.top, width(tr), height(tr)}, fit);
        const float s = dest.width / bs.width;
        const D2D1_RECT_F drawn = fit == ThumbFit::Contain ? rect(dest.left, dest.top, dest.width, dest.height) : tr;
        if (!bmp_brush_) {
            const auto bp = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_LINEAR);
            dc_->CreateBitmapBrush(bmp, &bp, nullptr, &bmp_brush_);
        } else {
            bmp_brush_->SetBitmap(bmp);
        }
        if (bmp_brush_) {
            bmp_brush_->SetOpacity(opacity_);
            bmp_brush_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(dest.left, dest.top));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(drawn, radius, radius), bmp_brush_.get());
        }
    }

    /// Plain (unclipped, square) bitmap draw with high-quality resampling.
    void bitmap(ID2D1Bitmap* bmp, D2D1_RECT_F dest) {
        dc_->DrawBitmap(bmp, &dest, opacity_, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    }

    /// Height of wrapped text in `w` DIPs.
    float measure_height(std::wstring_view s, Font f, float w) {
        return measure_wrapped(s, f, w).height;
    }

    /// Width and height of wrapped text constrained to `w` DIPs.
    D2D1_SIZE_F measure_wrapped(std::wstring_view s, Font f, float w) {
        if (s.empty()) return {};
        wil::com_ptr<IDWriteTextLayout> layout = wrapped_layout(s, f, w);
        if (!layout) return {};
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return {m.widthIncludingTrailingWhitespace, m.height};
    }

    void push_clip(D2D1_RECT_F r) {
        dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        if (!clips_.empty()) {
            const D2D1_RECT_F& c = clips_.back();
            r.left = std::max(r.left, c.left);
            r.top = std::max(r.top, c.top);
            r.right = std::min(r.right, c.right);
            r.bottom = std::min(r.bottom, c.bottom);
        }
        clips_.push_back(r);
    }
    void pop_clip() {
        dc_->PopAxisAlignedClip();
        if (!clips_.empty()) clips_.pop_back();
    }

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
    static DWRITE_TEXT_ALIGNMENT text_alignment(Align a) noexcept {
        return a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING;
    }

    wil::com_ptr<IDWriteTextLayout> wrapped_layout(std::wstring_view s, Font f, float w) {
        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(fonts_.dwrite()->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), fonts_.get(f), std::max(1.0f, w), 10000.0f, &layout))) return nullptr;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        const DWRITE_TRIMMING no_trim{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&no_trim, nullptr);
        return layout;
    }

    ID2D1DeviceContext6* dc_;
    Fonts& fonts_;
    std::vector<Hit>& hits_;
    float opacity_ = 1.0f;
    std::vector<D2D1_RECT_F> clips_;
    wil::com_ptr<ID2D1SolidColorBrush> brush_;
    wil::com_ptr<ID2D1BitmapBrush1> bmp_brush_;
};

inline const Hit* hit_test(const std::vector<Hit>& hits, D2D1_POINT_2F p) {
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        if (contains(it->r, p)) return &*it;
    }
    return nullptr;
}

/// Truncates a directory path in the middle while keeping the drive/root and deepest folder names visible.
inline std::wstring middle_truncate_dir(Canvas& cv, std::wstring_view path, Font f, float max_w) {
    if (path.empty() || max_w <= 0.0f || cv.measure(path, f) <= max_w) return std::wstring{path};

    size_t root_end = 0;
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') {
        root_end = 3;
    } else if (path.starts_with(L"\\\\")) {
        size_t slashes = 0;
        for (size_t i = 2; i < path.size(); ++i) {
            if (path[i] == L'\\') {
                ++slashes;
                if (slashes == 2) {
                    root_end = i + 1;
                    break;
                }
            }
        }
        if (root_end == 0) root_end = 2;
    }

    const std::wstring_view root = path.substr(0, root_end);
    std::wstring best;
    for (size_t i = path.size(); i > root_end; --i) {
        if (path[i - 1] != L'\\') continue;
        std::wstring cand{root};
        cand += L'…';
        cand += path.substr(i - 1);
        if (cv.measure(cand, f) <= max_w) best = std::move(cand);
        else break;
    }
    if (!best.empty()) return best;

    size_t lo = 0, hi = path.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        const size_t left_n = std::max<size_t>(root_end, mid / 3);
        const size_t right_n = mid > left_n ? mid - left_n : 0;
        if (left_n + right_n >= path.size()) {
            hi = mid - 1;
            continue;
        }
        std::wstring cand{path.substr(0, left_n)};
        cand += L'…';
        cand += path.substr(path.size() - right_n);
        if (cv.measure(cand, f) <= max_w) lo = mid;
        else hi = mid - 1;
    }
    const size_t left_n = std::min(path.size(), std::max<size_t>(root_end, lo / 3));
    const size_t right_n = lo > left_n ? std::min(path.size() - left_n, lo - left_n) : 0;
    std::wstring out{path.substr(0, left_n)};
    out += L'…';
    out += path.substr(path.size() - right_n);
    return out;
}

/// Tracks current and outgoing hover state with smooth exponential fades and zero idle repaints.
struct HoverTracker {
    int id = 0;
    int index = 0;
    int prev_id = 0;
    int prev_index = 0;
    AnimFloat in_anim;
    AnimFloat out_anim;

    bool set(int new_id, int new_index, bool animate) noexcept {
        if (new_id == id && new_index == index) return false;
        if (id != 0 && in_anim.value > 0.002f) {
            prev_id = id;
            prev_index = index;
            out_anim.snap(in_anim.value);
            out_anim.set(0.0f, animate);
        }
        id = new_id;
        index = new_index;
        in_anim.snap(0.0f);
        in_anim.set(new_id != 0 ? 1.0f : 0.0f, animate);
        if (!animate) {
            prev_id = 0;
            out_anim.snap(0.0f);
        }
        return true;
    }

    bool step(float dt) noexcept {
        bool moving = false;
        if (in_anim.step(dt, 34.0f, 0.008f)) moving = true;
        if (out_anim.step(dt, 28.0f, 0.008f)) moving = true;
        if (out_anim.value <= 0.008f) prev_id = 0;
        return moving;
    }

    [[nodiscard]] float alpha(int q_id, int q_index = 0) const noexcept {
        if (id == q_id && index == q_index) return in_anim.value;
        if (prev_id == q_id && prev_index == q_index) return out_anim.value;
        return 0.0f;
    }
};

/// Shared delayed-hover tooltip state with smooth fade-in/out and zero idle repaints.
struct HoverTooltip {
    static constexpr UINT DELAY_MS = 380;
    D2D1_RECT_F anchor{};
    D2D1_RECT_F pending_anchor{};
    std::wstring title;
    std::wstring subtitle;
    bool shown = false;
    AnimFloat alpha;

    void on_hover_changed(HWND hwnd, UINT_PTR timer_id, const Hit* h, bool eligible, bool animate) noexcept {
        if (hwnd) KillTimer(hwnd, timer_id);
        if (shown) {
            shown = false;
            alpha.set(0.0f, animate);
        } else if (!animate) {
            alpha.snap(0.0f);
        }
        pending_anchor = h ? h->r : D2D1_RECT_F{};
        if (eligible && hwnd) SetTimer(hwnd, timer_id, DELAY_MS, nullptr);
    }

    bool on_timer(HWND hwnd, UINT_PTR timer_id, bool allow, std::wstring new_title, std::wstring new_subtitle, bool animate) {
        if (hwnd) KillTimer(hwnd, timer_id);
        if (!allow || (new_title.empty() && new_subtitle.empty())) return false;
        anchor = pending_anchor;
        title = std::move(new_title);
        subtitle = std::move(new_subtitle);
        shown = true;
        alpha.snap(0.0f);
        alpha.set(1.0f, animate);
        return true;
    }

    void clear(HWND hwnd, UINT_PTR timer_id, bool animate = false) noexcept {
        if (hwnd) KillTimer(hwnd, timer_id);
        shown = false;
        alpha.set(0.0f, animate);
    }

    bool step(float dt) noexcept {
        return alpha.step(dt, shown ? 32.0f : 36.0f, 0.008f);
    }
};

/// Draws a floating wrapped tooltip anchored near `anchor`, clamped within `win_sz`.
inline void draw_tooltip(Canvas& cv, D2D1_SIZE_F win_sz, D2D1_RECT_F anchor, std::wstring_view title,
                         std::wstring_view subtitle = {}, float opacity = 1.0f) {
    if (opacity <= 0.008f || (title.empty() && subtitle.empty())) return;
    const Canvas::ScopedOpacity op{cv, opacity};
    constexpr float pad_x = 10.0f;
    constexpr float pad_y = 6.0f;
    constexpr float gap = 2.0f;
    const float max_w = std::min(460.0f, std::max(140.0f, win_sz.width - 16.0f));
    const float text_max_w = std::max(40.0f, max_w - pad_x * 2.0f);

    const Font title_font = subtitle.empty() ? Font::SmallWrap : Font::SmallSemi;
    const D2D1_SIZE_F ts = title.empty() ? D2D1_SIZE_F{} : cv.measure_wrapped(title, title_font, text_max_w);
    const D2D1_SIZE_F ss = subtitle.empty() ? D2D1_SIZE_F{} : cv.measure_wrapped(subtitle, Font::MonoSmall, text_max_w);

    const float content_w = std::max(ts.width, ss.width);
    const float content_h = ts.height + (!title.empty() && !subtitle.empty() ? gap : 0.0f) + ss.height;
    const float box_w = std::ceil(content_w) + pad_x * 2.0f;
    const float box_h = std::ceil(content_h) + pad_y * 2.0f;

    float x = (anchor.left + anchor.right - box_w) * 0.5f;
    x = std::clamp(x, 8.0f, std::max(8.0f, win_sz.width - 8.0f - box_w));

    float y = anchor.bottom + 8.0f;
    if (y + box_h > win_sz.height - 8.0f && anchor.top - box_h - 8.0f >= 8.0f) {
        y = anchor.top - box_h - 8.0f;
    }
    y = std::clamp(y, 8.0f, std::max(8.0f, win_sz.height - 8.0f - box_h));

    const D2D1_RECT_F r = rect(x, y, box_w, box_h);
    cv.shadow(r, 8.0f, 10.0f, 0.38f);
    cv.fill_round(r, 8.0f, color::surface_highest);
    cv.stroke_round(r, 8.0f, with_alpha(color::outline_variant, 0.85f));

    float ty = y + pad_y;
    if (!title.empty()) {
        cv.text_wrap(title, rect(x + pad_x, ty, text_max_w, ts.height + 2.0f), title_font, color::on_surface);
        ty += ts.height + gap;
    }
    if (!subtitle.empty()) {
        cv.text_wrap(subtitle, rect(x + pad_x, ty, text_max_w, ss.height + 2.0f), Font::MonoSmall, color::on_surface_variant);
    }
}

} // namespace usyseek::ui
