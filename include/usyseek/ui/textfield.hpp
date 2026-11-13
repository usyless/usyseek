#pragma once

#include "canvas.hpp"
#include "text_buffer.hpp"

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

/// Single-line editable text with selection, word navigation, undo/redo, clipboard and IME positioning.
class TextField : public TextBuffer {
public:
    std::wstring placeholder;
    bool focused = false;
    bool caret_on = true;

    void set(std::wstring s) {
        TextBuffer::set(std::move(s));
        scroll_ = 0.0f;
    }

    /// Returns true when the text changed. `handled` reports whether the key was consumed.
    bool on_key(HWND hwnd, UINT vk, bool& handled) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const bool alt = GetKeyState(VK_MENU) < 0;
        handled = true;
        switch (vk) {
            case VK_LEFT: move_left(ctrl, shift); return false;
            case VK_RIGHT: move_right(ctrl, shift); return false;
            case VK_HOME: move_home(shift); return false;
            case VK_END: move_end(shift); return false;
            case VK_BACK: return backspace(ctrl);
            case VK_DELETE: return del(ctrl);
        }
        if (ctrl && !alt) {
            switch (vk) {
                case 'A': select_all(); return false;
                case 'C':
                    if (has_selection()) set_clipboard_text(hwnd, selection());
                    return false;
                case 'X':
                    if (!has_selection()) return false;
                    set_clipboard_text(hwnd, selection());
                    return replace_selection({});
                case 'V': {
                    std::wstring clip = clipboard_text(hwnd);
                    std::erase_if(clip, [](wchar_t c) { return c == L'\r' || c == L'\n' || c == L'\t'; });
                    if (clip.empty()) return false;
                    return replace_selection(clip);
                }
                case 'Z': return shift ? redo() : undo();
                case 'Y':
                    if (!shift) return redo();
                    break;
            }
        }
        handled = false;
        return false;
    }

    void draw(Canvas& cv, D2D1_RECT_F r, Font f, D2D1_COLOR_F col, D2D1_COLOR_F placeholder_col) {
        rect_ = r;
        font_ = f;
        const bool visible_in_clip = cv.intersects_clip(r, 2.0f);
        if (!visible_in_clip && !focused && !has_selection()) return;

        IDWriteTextLayout* layout = ensure_layout(cv);
        if (!layout) return;

        float caret_x = 0.0f, caret_y = 0.0f;
        DWRITE_HIT_TEST_METRICS hm{};
        layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &caret_x, &caret_y, &hm);
        // Keep the caret visible
        const float w = width(r);
        if (caret_x - scroll_ > w - 2.0f) scroll_ = caret_x - w + 2.0f;
        if (caret_x - scroll_ < 0.0f) scroll_ = caret_x;
        scroll_ = std::max(0.0f, scroll_);
        caret_screen_x_ = r.left + caret_x - scroll_;
        if (!visible_in_clip) return;

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
        cv.dc()->DrawTextLayout(D2D1::Point2F(r.left - scroll_, r.top), layout, cv.brush(col), D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (focused && caret_on) {
            const float x = std::round(r.left + caret_x - scroll_);
            const float cy0 = r.top + hm.top + 1.0f;
            const float cy1 = r.top + hm.top + hm.height - 1.0f;
            cv.fill(D2D1::RectF(x, cy0, x + 1.5f, cy1), color::primary);
        }
        cv.pop_clip();
    }

    /// Mouse placement; `extend` keeps the anchor (shift-click / drag).
    void click(Canvas& cv, float x, bool extend) {
        IDWriteTextLayout* layout = ensure_layout(cv);
        if (!layout) return;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        layout->HitTestPoint(x - rect_.left + scroll_, 1.0f, &trailing, &inside, &m);
        move_to(static_cast<size_t>(m.textPosition) + (trailing ? static_cast<size_t>(m.length) : 0), extend);
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
    IDWriteTextLayout* ensure_layout(Canvas& cv) {
        IDWriteFactory3* dw = cv.fonts().dwrite();
        const float h = std::max(1.0f, height(rect_));
        if (cached_layout_ && cached_dwrite_ == dw && cached_font_ == font_ && cached_h_ == h && cached_text_ == text) {
            return cached_layout_.get();
        }
        wil::com_ptr<IDWriteTextLayout> layout;
        IDWriteTextFormat* fmt = cv.fonts().get(font_);
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 100000.0f, h, &layout);
        if (layout) {
            const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
            layout->SetTrimming(&none, nullptr);
        }
        cached_layout_ = std::move(layout);
        cached_dwrite_ = dw;
        cached_font_ = font_;
        cached_h_ = h;
        cached_text_ = text;
        return cached_layout_.get();
    }

    D2D1_RECT_F rect_{};
    Font font_ = Font::Body;
    float scroll_ = 0.0f;
    float caret_screen_x_ = 0.0f;
    IDWriteFactory3* cached_dwrite_ = nullptr;
    Font cached_font_ = Font::Body;
    float cached_h_ = 0.0f;
    std::wstring cached_text_;
    wil::com_ptr<IDWriteTextLayout> cached_layout_;
};

} // namespace usyseek::ui
