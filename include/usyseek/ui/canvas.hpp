#pragma once

#include "anim.hpp"
#include "fit.hpp"
#include "gfx.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <ranges>

namespace usyseek::ui {

/// Stable per-tag accent colour from the active theme.
[[nodiscard]] inline D2D1_COLOR_F tag_color(const Theme& t, std::string_view tag) noexcept {
    return t.tag_palette[rapidhash(tag.data(), tag.size()) % t.tag_palette.size()];
}

[[nodiscard]] inline D2D1_RECT_F lerp_rect(D2D1_RECT_F a, D2D1_RECT_F b, float t) noexcept {
    return {.left = lerp(a.left, b.left, t), .top = lerp(a.top, b.top, t), .right = lerp(a.right, b.right, t), .bottom = lerp(a.bottom, b.bottom, t)};
}

/// Hover multiplier for a segment while a sliding selection indicator covers `coverage` (0..1) of it.
/// Stays at full strength until the indicator covers ~3/4 of the segment, then eases out, so the hover never pops.
[[nodiscard]] inline float indicator_hover_fade(float coverage) noexcept {
    const float x = std::clamp((1.0f - coverage) * 4.0f, 0.0f, 1.0f);
    return x * x * (3.0f - (2.0f * x));
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
inline constexpr wchar_t info = 0xE946;
inline constexpr wchar_t zoom = 0xE8A3;
inline constexpr wchar_t link = 0xE71B;
inline constexpr wchar_t chevron_left = 0xE76B;
inline constexpr wchar_t chevron_right = 0xE76C;
inline constexpr wchar_t volume = 0xE767;
inline constexpr wchar_t mute = 0xE74F;
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
    Count,
};

enum class Align : uint8_t { Left, Center, Right };

class Fonts {
public:
    bool init(IDWriteFactory3* dw) {
        dw_ = dw;
        wil::com_ptr<IDWriteFontCollection> sys;
        if (dw_) dw_->GetSystemFontCollection(&sys);
        const std::wstring text = pick(sys.get(), L"Segoe UI Variable Text", L"Segoe UI");
        const std::wstring disp = pick(sys.get(), L"Segoe UI Variable Display", L"Segoe UI");
        const std::wstring mono = pick(sys.get(), L"Cascadia Mono", L"Consolas");
        const std::wstring icon = pick(sys.get(), L"Segoe Fluent Icons", L"Segoe MDL2 Assets");

        struct Def {
            Font f;
            const std::wstring* family;
            float size;
            DWRITE_FONT_WEIGHT weight;
            bool wrap;
            bool is_icon;
        };
        const std::array<Def, 17> defs = {{
            {.f = Font::Body, .family = &text, .size = 13.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::BodySemi, .family = &text, .size = 13.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::BodyWrap, .family = &text, .size = 13.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = true, .is_icon = false},
            {.f = Font::Large, .family = &disp, .size = 15.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::Input, .family = &text, .size = 17.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::Small, .family = &text, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::SmallSemi, .family = &text, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::SmallWrap, .family = &text, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = true, .is_icon = false},
            {.f = Font::Caption, .family = &text, .size = 11.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::Label, .family = &text, .size = 11.0f, .weight = DWRITE_FONT_WEIGHT_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::Mono, .family = &mono, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::MonoSmall, .family = &mono, .size = 11.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::Title, .family = &disp, .size = 17.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::Hero, .family = &disp, .size = 32.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::Icon, .family = &icon, .size = 16.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = true},
            {.f = Font::IconSmall, .family = &icon, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = true},
            {.f = Font::IconLarge, .family = &icon, .size = 20.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = true},
        },};
        for (const auto& d : defs) {
            wil::com_ptr<IDWriteTextFormat> fmt;
            if (FAILED(dw->CreateTextFormat(d.family->c_str(), nullptr, d.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, d.size, L"en-us", &fmt))) return false;
            fmt->SetWordWrapping(d.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            fmt->SetParagraphAlignment(d.wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (!d.wrap) {
                wil::com_ptr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.get(), &ellipsis))) {
                    const DWRITE_TRIMMING trim{.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER, .delimiter = 0, .delimiterCount = 0};
                    fmt->SetTrimming(&trim, ellipsis.get());
                }
            }
            if (!d.is_icon) center_cap_metrics(sys.get(), fmt.get(), *d.family, d.weight, d.size);
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

    struct WrappedEntry {
        std::wstring text;
        Font font = Font::Body;
        float width = 0.0f;
        Align align = Align::Left;
        D2D1_SIZE_F metrics{};
        wil::com_ptr<IDWriteTextLayout> layout;
    };

    /// Returns a cached word-wrapped DirectWrite layout and its measured bounds for `(s, f, w)`.
    [[nodiscard]] WrappedEntry* wrapped(std::wstring_view s, Font f, float w) {
        if (s.empty() || !dw_) return nullptr;
        const float cw = w > 1.0f ? w : 1.0f;
        const auto wbits = std::bit_cast<uint32_t>(cw);
        const uint64_t key = rapidhash(s.data(), s.size() * sizeof(wchar_t)) ^
                             (static_cast<uint64_t>(f) * 0x9e3779b97f4a7c15ull) ^
                             (static_cast<uint64_t>(wbits) * 0xbf58476d1ce4e5b9ull);
        if (const auto it = wrapped_cache_.find(key); it != wrapped_cache_.end()) {
            WrappedEntry& e = it->second;
            if (e.text == s && e.font == f && e.width == cw) return &e;
        }
        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(dw_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), get(f), cw, 10000.0f, &layout))) {
            return nullptr;
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const DWRITE_TRIMMING no_trim{.granularity = DWRITE_TRIMMING_GRANULARITY_NONE, .delimiter = 0, .delimiterCount = 0};
        layout->SetTrimming(&no_trim, nullptr);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        if (wrapped_cache_.size() >= 512) wrapped_cache_.clear();
        auto [it, inserted] = wrapped_cache_.insert_or_assign(
            key, WrappedEntry{
                     .text = std::wstring{s},
                     .font = f,
                     .width = cw,
                     .align = Align::Left,
                     .metrics = D2D1_SIZE_F{.width = m.widthIncludingTrailingWhitespace, .height = m.height},
                     .layout = std::move(layout),
                 });
        return &it->second;
    }

private:
    static std::wstring pick(IDWriteFontCollection* sys, const wchar_t* preferred, const wchar_t* fallback) {
        if (sys) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(preferred, &idx, &exists)) && exists) return preferred;
        }
        return fallback;
    }

    /// Balances DirectWrite's line baseline so cap height sits symmetrically inside single-line boxes
    /// instead of looking shifted downward by Segoe / Cascadia Mono's extra ascent headroom.
    void center_cap_metrics(IDWriteFontCollection* sys, IDWriteTextFormat* fmt, const std::wstring& family, DWRITE_FONT_WEIGHT weight, float size) const noexcept {
        if (!sys || !fmt || !dw_) return;
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (FAILED(sys->FindFamilyName(family.c_str(), &idx, &exists)) || !exists) return;
        wil::com_ptr<IDWriteFontFamily> fam;
        wil::com_ptr<IDWriteFont> font;
        if (FAILED(sys->GetFontFamily(idx, &fam)) ||
            FAILED(fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) {
            return;
        }
        DWRITE_FONT_METRICS fm{};
        font->GetMetrics(&fm);
        if (fm.designUnitsPerEm == 0 || fm.capHeight == 0) return;

        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(dw_->CreateTextLayout(L"H", 1, fmt, 1000.0f, 1000.0f, &layout))) return;
        DWRITE_LINE_METRICS lm{};
        UINT32 count = 0;
        if (FAILED(layout->GetLineMetrics(&lm, 1, &count)) || count == 0 || lm.height <= 0.0f) return;

        const float cap_h = static_cast<float>(fm.capHeight) * size / static_cast<float>(fm.designUnitsPerEm);
        if (lm.height <= cap_h) return;
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lm.height, (lm.height + cap_h) * 0.5f);
    }

    IDWriteFactory3* dw_ = nullptr;
    std::array<wil::com_ptr<IDWriteTextFormat>, static_cast<size_t>(Font::Count)> formats_;
    std::unordered_map<uint64_t, float> width_cache_;
    std::unordered_map<uint64_t, WrappedEntry> wrapped_cache_;
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

/// Returns true iff the axis-aligned rectangle `sub` lies completely inside the rounded rectangle `(r, radius)`.
[[nodiscard]] inline bool rect_inside_rounded_rect(D2D1_RECT_F sub, D2D1_RECT_F r, float radius) noexcept {
    if (sub.left < r.left || sub.top < r.top || sub.right > r.right || sub.bottom > r.bottom) return false;
    if (radius <= 0.0f) return true;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float hw = (r.right - r.left) * 0.5f;
    const float hh = (r.bottom - r.top) * 0.5f;
    const float cr = std::min({radius, hw, hh});
    const float dx = std::max(std::abs(sub.left - cx), std::abs(sub.right - cx));
    const float dy = std::max(std::abs(sub.top - cy), std::abs(sub.bottom - cy));
    const float qx = std::max(0.0f, dx - (hw - cr));
    const float qy = std::max(0.0f, dy - (hh - cr));
    return ((qx * qx) + (qy * qy)) <= (cr * cr);
}

/// Immediate-mode drawing helpers over one device context, plus hit-region recording.
class Canvas {
public:
    Canvas(WindowTarget& target, Fonts& fonts, std::vector<Hit>& hits, const Theme& theme, // NOLINT(cppcoreguidelines-pro-type-member-init)
           std::optional<D2D1_RECT_F> dirty_clip = std::nullopt, float opacity = 1.0f) noexcept
        : target_{target}, dc_{target.dc()}, fonts_{fonts}, hits_{hits}, theme_{theme},
          dirty_clip_{dirty_clip}, opacity_{std::clamp(opacity, 0.0f, 1.0f)} {}

    [[nodiscard]] ID2D1DeviceContext6* dc() const noexcept { return dc_; }
    [[nodiscard]] Fonts& fonts() const noexcept { return fonts_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] D2D1_COLOR_F tag_color(std::string_view tag) const noexcept { return usyseek::ui::tag_color(theme_, tag); }
    [[nodiscard]] bool has_dirty_clip() const noexcept { return dirty_clip_.has_value(); }
    [[nodiscard]] const std::optional<D2D1_RECT_F>& dirty_clip() const noexcept { return dirty_clip_; }
    [[nodiscard]] float scale() const noexcept {
        const float s = target_.scale();
        return s > 0.0f ? s : 1.0f;
    }
    [[nodiscard]] float snap(float v) const noexcept {
        const float s = scale();
        return std::round(v * s) / s;
    }
    [[nodiscard]] D2D1_RECT_F snap(D2D1_RECT_F r) const noexcept {
        const float s = scale();
        const float l = std::round(r.left * s);
        const float t = std::round(r.top * s);
        const float ri = r.right > r.left ? std::max(l + 1.0f, std::round(r.right * s)) : std::round(r.right * s);
        const float b = r.bottom > r.top ? std::max(t + 1.0f, std::round(r.bottom * s)) : std::round(r.bottom * s);
        return D2D1::RectF(l / s, t / s, ri / s, b / s);
    }
    /// Returns `{origin_dip, thickness_dip}` for a 1-DIP hairline aligned to whole device pixels.
    [[nodiscard]] std::pair<float, float> hairline(float v) const noexcept {
        const float s = scale();
        const float px = std::round(v * s);
        const float n = std::max(1.0f, std::round(s));
        return {px / s, n / s};
    }

    /// Returns true if `r` (expanded by `pad`) overlaps the dirty clip and active stack clip.
    [[nodiscard]] bool intersects_clip(D2D1_RECT_F r, float pad = 0.0f) const noexcept {
        const float l = r.left - pad;
        const float t = r.top - pad;
        const float ri = r.right + pad;
        const float b = r.bottom + pad;
        if (dirty_clip_) {
            if (ri <= dirty_clip_->left || l >= dirty_clip_->right || b <= dirty_clip_->top || t >= dirty_clip_->bottom) {
                return false;
            }
        }
        if (clip_depth_ > 0) {
            const D2D1_RECT_F& c = current_clip();
            if (ri <= c.left || l >= c.right || b <= c.top || t >= c.bottom) {
                return false;
            }
        }
        return true;
    }

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

    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) noexcept {
        c.a *= opacity_;
        if (target_.is_hdr()) {
            const float scale = target_.sdr_white_scale();
            c.r = srgb_to_linear_channel(c.r) * scale;
            c.g = srgb_to_linear_channel(c.g) * scale;
            c.b = srgb_to_linear_channel(c.b) * scale;
        }
        ID2D1SolidColorBrush* b = target_.solid_brush();
        if (b && (!has_color_ || c.r != last_color_.r || c.g != last_color_.g || c.b != last_color_.b || c.a != last_color_.a)) {
            b->SetColor(c);
            last_color_ = c;
            has_color_ = true;
        }
        return b;
    }

    void hit(D2D1_RECT_F r, int id, int index = 0) {
        if (clip_depth_ > 0) {
            const D2D1_RECT_F& c = current_clip();
            r.left = std::max(r.left, c.left);
            r.top = std::max(r.top, c.top);
            r.right = std::min(r.right, c.right);
            r.bottom = std::min(r.bottom, c.bottom);
        }
        if (r.left < r.right && r.top < r.bottom) {
            hits_.emplace_back(r, id, index);
        }
    }

    void fill(D2D1_RECT_F r, D2D1_COLOR_F c) noexcept {
        if (!intersects_clip(r, 1.0f)) return;
        dc_->FillRectangle(r, brush(c));
    }
    void fill_round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) noexcept {
        if (!intersects_clip(r, 1.0f)) return;
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush(c));
    }
    void stroke_round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float w = 1.0f) noexcept {
        if (!intersects_clip(r, (w * 0.5f) + 1.0f)) return;
        const float h = w * 0.5f;
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset(r, h, h), radius, radius), brush(c), w);
    }
    void fill_circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col) noexcept {
        if (!intersects_clip(D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), 1.0f)) return;
        dc_->FillEllipse(D2D1::Ellipse(c, r, r), brush(col));
    }
    void stroke_circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col, float w = 1.0f) noexcept {
        if (!intersects_clip(D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), (w * 0.5f) + 1.0f)) return;
        dc_->DrawEllipse(D2D1::Ellipse(c, r, r), brush(col), w);
    }
    /// Hairlines land on whole device pixels (a 1-DIP line is 1.5 px at 150%, which renders soft and uneven).
    void hline(float x0, float x1, float y, D2D1_COLOR_F c) noexcept {
        const auto [top, thick] = hairline(y);
        fill(D2D1::RectF(snap(x0), top, snap(x1), top + thick), c);
    }
    void vline(float x, float y0, float y1, D2D1_COLOR_F c) noexcept {
        const auto [left, thick] = hairline(x);
        fill(D2D1::RectF(left, snap(y0), left + thick, snap(y1)), c);
    }

    void text(std::wstring_view s, D2D1_RECT_F r, Font f, D2D1_COLOR_F c, Align a = Align::Left) noexcept {
        if (s.empty() || !intersects_clip(r, 1.0f)) return;
        IDWriteTextFormat* fmt = fonts_.get(f);
        fmt->SetTextAlignment(text_alignment(a));
        dc_->DrawText(s.data(), static_cast<UINT32>(s.size()), fmt, r, brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void text_wrap(std::wstring_view s, D2D1_RECT_F r, Font f, D2D1_COLOR_F c, Align a = Align::Left) {
        if (s.empty()) return;
        Fonts::WrappedEntry* e = fonts_.wrapped(s, f, width(r));
        if (!e || !e->layout) return;
        const D2D1_RECT_F bounds = D2D1::RectF(
            r.left, r.top,
            r.left + std::max(width(r), e->metrics.width),
            r.top + std::max(height(r), e->metrics.height));
        if (!intersects_clip(bounds, 2.0f)) return;
        if (e->align != a) {
            e->layout->SetTextAlignment(text_alignment(a));
            e->align = a;
        }
        dc_->DrawTextLayout(D2D1::Point2F(r.left, r.top), e->layout.get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void icon(wchar_t g, D2D1_RECT_F r, D2D1_COLOR_F c, Font f = Font::Icon) noexcept {
        const std::array<wchar_t, 2> s = {g, 0};
        text(std::wstring_view{s.data(), 1}, r, f, c, Align::Center);
    }

    float measure(std::wstring_view s, Font f) {
        return fonts_.measure(s, f);
    }

    void fill_bitmap_round(ID2D1Bitmap1* bmp, D2D1_RECT_F tr, float radius, ThumbFit fit = ThumbFit::Crop) noexcept {
        if (!bmp || !intersects_clip(tr, 1.0f)) return;
        const D2D1_SIZE_F bs = bmp->GetSize();
        if (bs.width <= 0.0f || bs.height <= 0.0f) return;
        const D2D1_RECT_F box = snap(tr);
        const FitRect dest = fit_image(bs.width, bs.height, FitRect{.left = box.left, .top = box.top, .width = width(box), .height = height(box)}, fit);
        const D2D1_RECT_F drawn = fit == ThumbFit::Contain ? snap(rect(dest.left, dest.top, dest.width, dest.height)) : box;
        const float sx = fit == ThumbFit::Contain ? (width(drawn) / bs.width) : (dest.width / bs.width);
        const float sy = fit == ThumbFit::Contain ? (height(drawn) / bs.height) : sx;
        const float tx = fit == ThumbFit::Contain ? drawn.left : dest.left;
        const float ty = fit == ThumbFit::Contain ? drawn.top : dest.top;
        const D2D1_MATRIX_3X2_F xf = D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(tx, ty);
        if (target_.is_hdr() && bmp->GetPixelFormat().format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            if (ID2D1ImageBrush* ib = target_.sdr_image_brush(bmp, D2D1::RectF(0.0f, 0.0f, bs.width, bs.height),
                                                              D2D1_INTERPOLATION_MODE_LINEAR)) {
                ib->SetOpacity(opacity_);
                ib->SetTransform(xf);
                dc_->FillRoundedRectangle(D2D1::RoundedRect(drawn, radius, radius), ib);
                return;
            }
        }
        if (ID2D1BitmapBrush1* bb = target_.bitmap_brush(bmp)) {
            bb->SetOpacity(opacity_);
            bb->SetTransform(xf);
            dc_->FillRoundedRectangle(D2D1::RoundedRect(drawn, radius, radius), bb);
        }
    }

    /// Plain (unclipped, square) bitmap draw with configurable resampling.
    void bitmap(ID2D1Bitmap* bmp, D2D1_RECT_F dest,
                D2D1_INTERPOLATION_MODE mode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC) noexcept {
        if (!bmp) return;
        dest = snap(dest);
        if (!intersects_clip(dest, 1.0f)) return;
        if (target_.is_hdr() && bmp->GetPixelFormat().format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const D2D1_SIZE_F bs = bmp->GetSize();
            if (bs.width > 0.0f && bs.height > 0.0f) {
                if (ID2D1ImageBrush* ib = target_.sdr_image_brush(bmp, D2D1::RectF(0.0f, 0.0f, bs.width, bs.height), mode)) {
                    ib->SetOpacity(opacity_);
                    ib->SetTransform(D2D1::Matrix3x2F::Scale(width(dest) / bs.width, height(dest) / bs.height) *
                                     D2D1::Matrix3x2F::Translation(dest.left, dest.top));
                    dc_->FillRectangle(dest, ib);
                    return;
                }
            }
        }
        dc_->DrawBitmap(bmp, &dest, opacity_, mode, nullptr, nullptr);
    }

    /// Draws `src` rectangle (in bitmap DIPs) mapped into `dest`.
    void bitmap_region(ID2D1Bitmap* bmp, D2D1_RECT_F dest, D2D1_RECT_F src,
                       D2D1_INTERPOLATION_MODE mode = D2D1_INTERPOLATION_MODE_LINEAR) noexcept {
        if (!bmp) return;
        dest = snap(dest);
        if (!intersects_clip(dest, 1.0f)) return;
        if (target_.is_hdr() && bmp->GetPixelFormat().format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const float sw = width(src);
            const float sh = height(src);
            if (sw > 0.0f && sh > 0.0f) {
                if (ID2D1ImageBrush* ib = target_.sdr_image_brush(bmp, src, mode)) {
                    ib->SetOpacity(opacity_);
                    ib->SetTransform(D2D1::Matrix3x2F::Translation(-src.left, -src.top) *
                                     D2D1::Matrix3x2F::Scale(width(dest) / sw, height(dest) / sh) *
                                     D2D1::Matrix3x2F::Translation(dest.left, dest.top));
                    dc_->FillRectangle(dest, ib);
                    return;
                }
            }
        }
        dc_->DrawBitmap(bmp, &dest, opacity_, mode, &src, nullptr);
    }

    /// Height of wrapped text in `w` DIPs.
    float measure_height(std::wstring_view s, Font f, float w) {
        return measure_wrapped(s, f, w).height;
    }

    /// Width and height of wrapped text constrained to `w` DIPs.
    D2D1_SIZE_F measure_wrapped(std::wstring_view s, Font f, float w) {
        const auto* e = fonts_.wrapped(s, f, w);
        return e ? e->metrics : D2D1_SIZE_F{};
    }

    void push_clip(D2D1_RECT_F r) noexcept {
        dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        record_clip_rect(r);
    }
    void pop_clip() noexcept {
        dc_->PopAxisAlignedClip();
        if (clip_depth_ > 0) --clip_depth_;
    }

    /// Pushes an anti-aliased rounded-rectangle layer mask (falls back to axis-aligned clip if `radius <= 0`
    /// or when the active dirty clip lies strictly inside the rounded interior).
    /// Returns true when a D2D layer was pushed and must be paired with `pop_round_clip(true)`.
    [[nodiscard]] bool push_round_clip(D2D1_RECT_F r, float radius) noexcept {
        if (radius <= 0.0f || (dirty_clip_ && rect_inside_rounded_rect(*dirty_clip_, inset(r, 1.0f, 1.0f), std::max(0.0f, radius - 1.0f)))) {
            push_clip(r);
            return false;
        }
        ID2D1RoundedRectangleGeometry* geom = target_.round_clip_geom(r, radius);
        if (!geom) {
            push_clip(r);
            return false;
        }
        D2D1_LAYER_PARAMETERS1 lp{};
        lp.contentBounds = r;
        if (dirty_clip_) {
            lp.contentBounds.left = std::max(lp.contentBounds.left, dirty_clip_->left);
            lp.contentBounds.top = std::max(lp.contentBounds.top, dirty_clip_->top);
            lp.contentBounds.right = std::min(lp.contentBounds.right, dirty_clip_->right);
            lp.contentBounds.bottom = std::min(lp.contentBounds.bottom, dirty_clip_->bottom);
        }
        lp.geometricMask = geom;
        lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        lp.maskTransform = D2D1::Matrix3x2F::Identity();
        lp.opacity = 1.0f;
        lp.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
        dc_->PushLayer(&lp, target_.clip_layer());
        record_clip_rect(r);
        return true;
    }

    void pop_round_clip(bool layered) noexcept {
        if (!layered) {
            pop_clip();
            return;
        }
        dc_->PopLayer();
        if (clip_depth_ > 0) --clip_depth_;
    }

    /// Soft drop shadow approximated with stacked translucent rounded rects (cheap, no effects).
    void shadow(D2D1_RECT_F r, float radius, float spread = 10.0f, float alpha = 0.28f) noexcept {
        const float scaled = alpha * theme_.shadow_alpha_scale;
        if (scaled <= 0.002f) return;
        const D2D1_RECT_F bounds = D2D1::RectF(r.left - (spread * 0.6f), r.top - (spread * 0.2f), r.right + (spread * 0.6f), r.bottom + spread);
        if (!intersects_clip(bounds, 1.0f)) return;
        constexpr int steps = 5;
        for (int i = steps; i >= 1; --i) {
            const float t = static_cast<float>(i) / steps;
            const float g = spread * t;
            fill_round(D2D1::RectF(r.left - (g * 0.6f), r.top - (g * 0.2f), r.right + (g * 0.6f), r.bottom + g), radius + g, hex(0x000000, scaled / steps * (1.2f - t)));
        }
    }

private:
    void record_clip_rect(D2D1_RECT_F r) noexcept {
        if (clip_depth_ > 0) {
            const D2D1_RECT_F& c = current_clip();
            r.left = std::max(r.left, c.left);
            r.top = std::max(r.top, c.top);
            r.right = std::min(r.right, c.right);
            r.bottom = std::min(r.bottom, c.bottom);
        }
        if (clip_depth_ < clips_.size()) {
            clips_[clip_depth_++] = r;
        } else {
            ++clip_depth_;
        }
    }

    [[nodiscard]] const D2D1_RECT_F& current_clip() const noexcept {
        return clips_[std::min(clip_depth_, clips_.size()) - 1];
    }

    [[nodiscard]] static float srgb_to_linear_channel(float x) noexcept {
        x = std::clamp(x, 0.0f, 1.0f);
        if (x <= 0.04045f) return x / 12.92f;
        return std::pow((x + 0.055f) / 1.055f, 2.4f);
    }

    static DWRITE_TEXT_ALIGNMENT text_alignment(Align a) noexcept {
        return a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING;
    }

    WindowTarget& target_;
    ID2D1DeviceContext6* dc_;
    Fonts& fonts_;
    std::vector<Hit>& hits_;
    const Theme& theme_;
    std::optional<D2D1_RECT_F> dirty_clip_;
    float opacity_ = 1.0f;
    D2D1_COLOR_F last_color_{};
    bool has_color_ = false;
    std::array<D2D1_RECT_F, 8> clips_; // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t clip_depth_ = 0;
};

inline const Hit* hit_test(std::span<const Hit> hits, D2D1_POINT_2F p) noexcept {
    for (const auto& h : std::views::reverse(hits)) {
        if (contains(h.r, p)) return &h;
    }
    return nullptr;
}

/// Truncates a directory path in the middle into `out`, reusing a single scratch buffer.
inline void middle_truncate_dir_into(Canvas& cv, std::wstring_view path, Font f, float max_w, std::wstring& out) {
    if (path.empty() || max_w <= 0.0f || cv.measure(path, f) <= max_w) {
        out.assign(path);
        return;
    }

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
    out.clear();
    std::wstring cand;
    for (size_t i = path.size(); i > root_end; --i) {
        if (path[i - 1] != L'\\') continue;
        cand.assign(root);
        cand.push_back(L'…');
        cand.append(path.substr(i - 1));
        if (cv.measure(cand, f) <= max_w) {
            out.assign(cand);
        } else {
            break;
        }
    }
    if (!out.empty()) return;

    size_t lo = 0;
    size_t hi = path.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        const size_t left_n = std::max<size_t>(root_end, mid / 3);
        const size_t right_n = mid > left_n ? mid - left_n : 0;
        if (left_n + right_n >= path.size()) {
            hi = mid - 1;
            continue;
        }
        cand.assign(path.substr(0, left_n));
        cand.push_back(L'…');
        cand.append(path.substr(path.size() - right_n));
        if (cv.measure(cand, f) <= max_w) { 
            lo = mid;
        } else { 
            hi = mid - 1;
        }
    }
    const size_t left_n = std::min(path.size(), std::max<size_t>(root_end, lo / 3));
    const size_t right_n = lo > left_n ? std::min(path.size() - left_n, lo - left_n) : 0;
    out.assign(path.substr(0, left_n));
    out.push_back(L'…');
    out.append(path.substr(path.size() - right_n));
}

/// Truncates a directory path in the middle while keeping the drive/root and deepest folder names visible.
inline std::wstring middle_truncate_dir(Canvas& cv, std::wstring_view path, Font f, float max_w) {
    std::wstring out;
    middle_truncate_dir_into(cv, path, f, max_w, out);
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

    bool on_timer(HWND hwnd, UINT_PTR timer_id, bool allow, std::wstring new_title, std::wstring new_subtitle, bool animate) noexcept {
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

struct TooltipLayout {
    D2D1_RECT_F box{};
    D2D1_RECT_F dirty{};
    D2D1_SIZE_F title_sz{};
    D2D1_SIZE_F sub_sz{};
    float text_max_w = 0.0f;
    Font title_font = Font::SmallWrap;
    bool valid = false;
};

/// Computes the screen-space box and shadow-inflated dirty rect for a tooltip anchored near `anchor`,
/// clamped within `bounds` (and inset from any rounded corners of radius `bounds_radius`).
[[nodiscard]] inline TooltipLayout measure_tooltip_layout(Fonts& fonts, D2D1_RECT_F bounds, D2D1_RECT_F anchor,
                                                          std::wstring_view title, std::wstring_view subtitle,
                                                          float bounds_radius = 0.0f) {
    TooltipLayout out;
    if (title.empty() && subtitle.empty()) return out;
    constexpr float pad_x = 10.0f;
    constexpr float pad_y = 6.0f;
    constexpr float gap = 2.0f;
    const float max_w = std::min(460.0f, std::max(140.0f, width(bounds) - 16.0f));
    out.text_max_w = std::max(40.0f, max_w - (pad_x * 2.0f));
    out.title_font = subtitle.empty() ? Font::SmallWrap : Font::SmallSemi;
    if (!title.empty()) {
        if (const auto* e = fonts.wrapped(title, out.title_font, out.text_max_w)) out.title_sz = e->metrics;
    }
    if (!subtitle.empty()) {
        if (const auto* e = fonts.wrapped(subtitle, Font::MonoSmall, out.text_max_w)) out.sub_sz = e->metrics;
    }
    const float content_w = std::max(out.title_sz.width, out.sub_sz.width);
    const float content_h = out.title_sz.height + (!title.empty() && !subtitle.empty() ? gap : 0.0f) + out.sub_sz.height;
    const float box_w = std::ceil(content_w) + (pad_x * 2.0f);
    const float box_h = std::ceil(content_h) + (pad_y * 2.0f);

    const float min_y = bounds.top + 8.0f;
    const float max_y = std::max(min_y, bounds.bottom - 8.0f - box_h);
    float y = anchor.bottom + 8.0f;
    if (y + box_h > bounds.bottom - 8.0f && anchor.top - box_h - 8.0f >= min_y) {
        y = anchor.top - box_h - 8.0f;
    }
    y = std::clamp(y, min_y, max_y);

    // Keep the tooltip box inside rounded outer pill corners so neither the tooltip nor its glass blur samples outside.
    float edge_margin = 8.0f;
    const float cr = std::clamp(bounds_radius, 0.0f, std::min(width(bounds), height(bounds)) * 0.5f);
    if (cr > 0.0f) {
        const auto corner_inset = [cr](float d) noexcept {
            if (d >= cr) return 0.0f;
            const float dy = cr - std::max(0.0f, d);
            return cr - std::sqrt(std::max(0.0f, (cr * cr) - (dy * dy)));
        };
        const float arc = std::max(corner_inset(y - bounds.top), corner_inset(bounds.bottom - (y + box_h)));
        edge_margin = std::max(edge_margin, arc + 6.0f);
    }

    const float min_x = bounds.left + edge_margin;
    const float max_x = bounds.right - edge_margin - box_w;
    float x = (anchor.left + anchor.right - box_w) * 0.5f;
    if (max_x >= min_x) {
        x = std::clamp(x, min_x, max_x);
    } else {
        x = std::clamp(x, bounds.left + 8.0f, std::max(bounds.left + 8.0f, bounds.right - 8.0f - box_w));
    }

    out.box = rect(x, y, box_w, box_h);
    // Covers the 10.0 DIP drop shadow in Canvas::shadow(r, rad, 10.0f, 0.38f)
    out.dirty = D2D1::RectF(out.box.left - 6.0f, out.box.top - 2.0f, out.box.right + 6.0f, out.box.bottom + 10.0f);
    out.valid = true;
    return out;
}

/// Computes the screen-space box and shadow-inflated dirty rect for a tooltip anchored near `anchor`.
[[nodiscard]] inline TooltipLayout measure_tooltip_layout(Fonts& fonts, D2D1_SIZE_F win_sz, D2D1_RECT_F anchor,
                                                          std::wstring_view title, std::wstring_view subtitle) {
    return measure_tooltip_layout(fonts, D2D1::RectF(0.0f, 0.0f, win_sz.width, win_sz.height), anchor, title, subtitle, 0.0f);
}

/// Returns the tooltip corner radius derived from the active theme.
[[nodiscard]] inline float tooltip_corner_radius(const Theme& t, float box_h) noexcept {
    return std::min({box_h * 0.5f, 14.0f, std::max(0.0f, t.button_radius * 0.8f)});
}

/// Returns true when the active theme is the Liquid Glass theme.
[[nodiscard]] inline bool is_liquid_glass(const Theme& t) noexcept {
    return t.is_liquid_glass;
}

/// Draws a frosted-glass rounded surface with an even inner specular sheen and crisp glass border.
inline void draw_glass_box(Canvas& cv, D2D1_RECT_F r, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F border,
                           float border_w = 1.0f, float sheen_alpha = -1.0f) noexcept {
    if (fill.a > 0.0f) cv.fill_round(r, radius, fill);
    const float sa = sheen_alpha >= 0.0f ? sheen_alpha : (cv.theme().dark_mode ? 0.11f : 0.36f);
    if (sa > 0.002f && width(r) > 4.0f && height(r) > 4.0f) {
        cv.stroke_round(inset(r, 1.0f, 1.0f), std::max(0.0f, radius - 1.0f), with_alpha(hex(0xffffff), sa), 1.0f);
    }
    if (border.a > 0.0f && border_w > 0.0f) cv.stroke_round(r, radius, border, border_w);
}

namespace detail {

template <typename GlassFn>
inline void draw_tooltip_impl(Canvas& cv, D2D1_RECT_F bounds, D2D1_RECT_F anchor, std::wstring_view title,
                              std::wstring_view subtitle, float opacity, float bounds_radius, GlassFn&& draw_glass) {
    if (opacity <= 0.008f || (title.empty() && subtitle.empty())) return;
    const TooltipLayout tl = measure_tooltip_layout(cv.fonts(), bounds, anchor, title, subtitle, bounds_radius);
    if (!tl.valid || !cv.intersects_clip(tl.dirty, 1.0f)) return;
    const Canvas::ScopedOpacity op{cv, opacity};
    constexpr float pad_x = 10.0f;
    constexpr float pad_y = 6.0f;
    constexpr float gap = 2.0f;
    const D2D1_RECT_F r = tl.box;
    const Theme& t = cv.theme();
    const float rad = tooltip_corner_radius(t, height(r));
    cv.shadow(r, rad, 10.0f, 0.38f);
    if (is_liquid_glass(t)) {
        if (!draw_glass(r, t, rad, std::clamp(opacity, 0.0f, 1.0f))) {
            draw_glass_box(cv, r, rad,
                           with_alpha(t.surface_high, t.dark_mode ? 0.56f : 0.76f),
                           with_alpha(hex(0xffffff), t.dark_mode ? 0.24f : 0.68f));
        }
    } else {
        cv.fill_round(r, rad, t.surface_highest);
        cv.stroke_round(r, rad, with_alpha(t.outline_variant, 0.85f));
    }

    float ty = r.top + pad_y;
    if (!title.empty()) {
        cv.text_wrap(title, rect(r.left + pad_x, ty, tl.text_max_w, tl.title_sz.height + 2.0f), tl.title_font, t.on_surface);
        ty += tl.title_sz.height + gap;
    }
    if (!subtitle.empty()) {
        cv.text_wrap(subtitle, rect(r.left + pad_x, ty, tl.text_max_w, tl.sub_sz.height + 2.0f), Font::MonoSmall, t.on_surface_variant);
    }
}

} // namespace detail

/// Draws a floating wrapped tooltip anchored near `anchor`, clamped within `bounds`.
inline void draw_tooltip(Canvas& cv, D2D1_RECT_F bounds, D2D1_RECT_F anchor, std::wstring_view title,
                         std::wstring_view subtitle = {}, float opacity = 1.0f, float bounds_radius = 0.0f) {
    detail::draw_tooltip_impl(cv, bounds, anchor, title, subtitle, opacity, bounds_radius,
                              [](D2D1_RECT_F, const Theme&, float, float) noexcept { return false; });
}

/// Draws a floating wrapped tooltip anchored near `anchor`, clamped within `win_sz`.
inline void draw_tooltip(Canvas& cv, D2D1_SIZE_F win_sz, D2D1_RECT_F anchor, std::wstring_view title,
                         std::wstring_view subtitle = {}, float opacity = 1.0f) {
    draw_tooltip(cv, D2D1::RectF(0.0f, 0.0f, win_sz.width, win_sz.height), anchor, title, subtitle, opacity, 0.0f);
}

} // namespace usyseek::ui
