#pragma once

#include "canvas.hpp"

#include <imm.h>

namespace usyseek::ui {

inline std::wstring clipboard_text(HWND hwnd) {
    std::wstring out;
    if (!OpenClipboard(hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

inline void set_clipboard_text(HWND hwnd, std::wstring_view s) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
        auto* p = static_cast<wchar_t*>(GlobalLock(h));
        std::memcpy(p, s.data(), s.size() * sizeof(wchar_t));
        p[s.size()] = 0;
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

/// Single-line editable text with selection, word navigation, clipboard and IME positioning.
class TextField {
public:
    std::wstring text;
    std::wstring placeholder;
    size_t caret = 0;
    size_t anchor = 0;
    bool focused = false;
    bool caret_on = true;

    void set(std::wstring s) {
        text = std::move(s);
        caret = anchor = text.size();
        scroll_ = 0.0f;
    }
    void select_all() {
        anchor = 0;
        caret = text.size();
    }
    [[nodiscard]] bool has_selection() const noexcept { return caret != anchor; }
    [[nodiscard]] std::wstring_view selection() const {
        const auto [a, b] = sel();
        return std::wstring_view{text}.substr(a, b - a);
    }

    /// Returns true when the text changed.
    bool on_char(wchar_t c) {
        if (c < 0x20 || c == 0x7f) return false;
        replace_selection(std::wstring_view{&c, 1});
        return true;
    }

    /// Returns true when the text changed. `handled` reports whether the key was consumed.
    bool on_key(HWND hwnd, UINT vk, bool& handled) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        handled = true;
        auto move = [&](size_t to) {
            caret = to;
            if (!shift) anchor = caret;
        };
        switch (vk) {
            case VK_LEFT:
                if (!shift && has_selection()) move(sel().first);
                else move(ctrl ? word_left(caret) : (caret ? caret - 1 : 0));
                return false;
            case VK_RIGHT:
                if (!shift && has_selection()) move(sel().second);
                else move(ctrl ? word_right(caret) : std::min(text.size(), caret + 1));
                return false;
            case VK_HOME: move(0); return false;
            case VK_END: move(text.size()); return false;
            case VK_BACK:
                if (!has_selection()) {
                    if (caret == 0) return false;
                    anchor = ctrl ? word_left(caret) : caret - 1;
                }
                replace_selection({});
                return true;
            case VK_DELETE:
                if (!has_selection()) {
                    if (caret >= text.size()) return false;
                    anchor = ctrl ? word_right(caret) : caret + 1;
                }
                replace_selection({});
                return true;
        }
        if (ctrl) {
            switch (vk) {
                case 'A': select_all(); return false;
                case 'C':
                    if (has_selection()) set_clipboard_text(hwnd, selection());
                    return false;
                case 'X':
                    if (!has_selection()) return false;
                    set_clipboard_text(hwnd, selection());
                    replace_selection({});
                    return true;
                case 'V': {
                    std::wstring clip = clipboard_text(hwnd);
                    std::erase_if(clip, [](wchar_t c) { return c == L'\r' || c == L'\n' || c == L'\t'; });
                    if (clip.empty()) return false;
                    replace_selection(clip);
                    return true;
                }
            }
        }
        handled = false;
        return false;
    }

    void draw(Canvas& cv, D2D1_RECT_F r, Font f, D2D1_COLOR_F col, D2D1_COLOR_F placeholder_col) {
        rect_ = r;
        font_ = f;
        auto layout = make_layout(cv);
        if (!layout) return;

        float caret_x = 0.0f, caret_y = 0.0f;
        DWRITE_HIT_TEST_METRICS hm{};
        layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &caret_x, &caret_y, &hm);
        // Keep the caret visible
        const float w = width(r);
        if (caret_x - scroll_ > w - 2.0f) scroll_ = caret_x - w + 2.0f;
        if (caret_x - scroll_ < 0.0f) scroll_ = caret_x;
        scroll_ = std::max(0.0f, scroll_);

        cv.push_clip(D2D1::RectF(r.left - 1.0f, r.top, r.right + 2.0f, r.bottom));
        if (text.empty() && !placeholder.empty()) {
            cv.text(placeholder, r, f, placeholder_col);
        }
        if (has_selection() && focused) {
            const auto [a, b] = sel();
            UINT32 count = 0;
            layout->HitTestTextRange(static_cast<UINT32>(a), static_cast<UINT32>(b - a), 0, 0, nullptr, 0, &count);
            std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
            if (count && SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(a), static_cast<UINT32>(b - a), 0, 0, ranges.data(), count, &count))) {
                for (const auto& m : ranges) {
                    cv.fill(D2D1::RectF(r.left + m.left - scroll_, r.top + m.top, r.left + m.left + m.width - scroll_, r.top + m.top + m.height), with_alpha(color::primary, 0.35f));
                }
            }
        }
        cv.dc()->DrawTextLayout(D2D1::Point2F(r.left - scroll_, r.top), layout.get(), cv.brush(col), D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (focused && caret_on) {
            const float x = std::round(r.left + caret_x - scroll_);
            const float cy0 = r.top + hm.top + 1.0f;
            const float cy1 = r.top + hm.top + hm.height - 1.0f;
            cv.fill(D2D1::RectF(x, cy0, x + 1.5f, cy1), color::primary);
        }
        cv.pop_clip();
        caret_screen_x_ = r.left + caret_x - scroll_;
    }

    /// Mouse placement; `extend` keeps the anchor (shift-click / drag).
    void click(Canvas& cv, float x, bool extend) {
        auto layout = make_layout(cv);
        if (!layout) return;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        layout->HitTestPoint(x - rect_.left + scroll_, 1.0f, &trailing, &inside, &m);
        caret = std::min(text.size(), static_cast<size_t>(m.textPosition + (trailing ? m.length : 0)));
        if (!extend) anchor = caret;
    }

    void select_word_at_caret() {
        anchor = word_left(std::min(text.size(), caret + 1));
        caret = word_right(anchor);
    }

    /// Positions the IME candidate window under the caret (pixels = DIPs * scale).
    void place_ime(HWND hwnd, float scale) const {
        if (HIMC imc = ImmGetContext(hwnd)) {
            COMPOSITIONFORM cf{};
            cf.dwStyle = CFS_POINT;
            cf.ptCurrentPos = {static_cast<LONG>(caret_screen_x_ * scale), static_cast<LONG>(rect_.top * scale)};
            ImmSetCompositionWindow(imc, &cf);
            ImmReleaseContext(hwnd, imc);
        }
    }

    [[nodiscard]] const D2D1_RECT_F& rect() const noexcept { return rect_; }

private:
    [[nodiscard]] std::pair<size_t, size_t> sel() const noexcept { return {std::min(caret, anchor), std::max(caret, anchor)}; }

    void replace_selection(std::wstring_view s) {
        const auto [a, b] = sel();
        text.replace(a, b - a, s);
        caret = anchor = a + s.size();
    }

    [[nodiscard]] size_t word_left(size_t i) const {
        while (i > 0 && iswspace(text[i - 1])) --i;
        while (i > 0 && !iswspace(text[i - 1]) && !is_sep(text[i - 1])) --i;
        if (i > 0 && i == caret && is_sep(text[i - 1])) --i;
        return i;
    }
    [[nodiscard]] size_t word_right(size_t i) const {
        const size_t n = text.size();
        if (i < n && is_sep(text[i])) return i + 1;
        while (i < n && !iswspace(text[i]) && !is_sep(text[i])) ++i;
        while (i < n && iswspace(text[i])) ++i;
        return i;
    }
    static bool is_sep(wchar_t c) noexcept { return c == L'\\' || c == L'/' || c == L':' || c == L'.' || c == L'_' || c == L'-'; }

    wil::com_ptr<IDWriteTextLayout> make_layout(Canvas& cv) const {
        wil::com_ptr<IDWriteTextLayout> layout;
        IDWriteTextFormat* fmt = cv.fonts().get(font_);
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        cv.fonts().dwrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 100000.0f, std::max(1.0f, height(rect_)), &layout);
        if (layout) {
            const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
            layout->SetTrimming(&none, nullptr);
        }
        return layout;
    }

    D2D1_RECT_F rect_{};
    Font font_ = Font::Body;
    float scroll_ = 0.0f;
    float caret_screen_x_ = 0.0f;
};

} // namespace usyseek::ui
