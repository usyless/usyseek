#include "check.hpp"

#include <usyseek/ui/text_buffer.hpp>

#include <cstdio>

using usyseek::ui::TextBuffer;

namespace {

void type_str(TextBuffer& tb, std::wstring_view s) {
    for (const wchar_t c : s) CHECK(tb.on_char(c));
}

void run_text_buffer_tests() {
    // Empty buffer
    {
        TextBuffer tb;
        CHECK(!tb.can_undo());
        CHECK(!tb.can_redo());
        CHECK(!tb.undo());
        CHECK(!tb.redo());
    }

    // Word-coalesced typing, undo, redo, and redo invalidation on new edit
    {
        TextBuffer tb;
        type_str(tb, L"red circle");
        CHECK(tb.text == L"red circle");
        CHECK(tb.caret == 10);
        CHECK(tb.anchor == 10);

        CHECK(tb.undo());
        CHECK(tb.text == L"red ");
        CHECK(tb.caret == 4);
        CHECK(tb.anchor == 4);
        CHECK(tb.can_redo());

        CHECK(tb.undo());
        CHECK(tb.text.empty());
        CHECK(tb.caret == 0);
        CHECK(!tb.can_undo());

        CHECK(tb.redo());
        CHECK(tb.text == L"red ");
        CHECK(tb.caret == 4);

        // Typing a new word clears redo history
        type_str(tb, L"square");
        CHECK(tb.text == L"red square");
        CHECK(!tb.can_redo());
        CHECK(!tb.redo());

        CHECK(tb.undo());
        CHECK(tb.text == L"red ");
    }

    // Selection replacement by typing restores selection on undo
    {
        TextBuffer tb;
        tb.set(L"red circle");
        CHECK(!tb.can_undo()); // set() clears history
        tb.move_home(false);
        tb.move_right(false, true);
        tb.move_right(false, true);
        tb.move_right(false, true);
        CHECK(tb.selection() == L"red");

        type_str(tb, L"blue");
        CHECK(tb.text == L"blue circle");
        CHECK(tb.caret == 4);

        CHECK(tb.undo());
        CHECK(tb.text == L"red circle");
        CHECK(tb.anchor == 0);
        CHECK(tb.caret == 3);
        CHECK(tb.selection() == L"red");
    }

    // Backspace coalescing and Ctrl+Backspace discrete step
    {
        TextBuffer tb;
        tb.set(L"one two");
        CHECK(tb.backspace(false)); // 'o'
        CHECK(tb.backspace(false)); // 'w'
        CHECK(tb.backspace(false)); // 't'
        CHECK(tb.backspace(false)); // ' '
        CHECK(tb.text == L"one");
        CHECK(tb.backspace(false)); // 'e'
        CHECK(tb.text == L"on");

        CHECK(tb.undo());
        CHECK(tb.text == L"one");
        CHECK(tb.undo());
        CHECK(tb.text == L"one two");
        CHECK(tb.caret == 7);
        CHECK(tb.anchor == 7);

        // Ctrl+Backspace deletes one word discretely and restores caret without selection
        CHECK(tb.backspace(true));
        CHECK(tb.text == L"one ");
        CHECK(tb.undo());
        CHECK(tb.text == L"one two");
        CHECK(!tb.has_selection());
        CHECK(tb.caret == 7);
    }

    // Forward delete coalescing and Ctrl+Delete
    {
        TextBuffer tb;
        tb.set(L"alpha beta");
        tb.move_home(false);
        for (int i = 0; i < 6; ++i) CHECK(tb.del(false)); // "alpha "
        CHECK(tb.text == L"beta");
        CHECK(tb.del(false)); // 'b'
        CHECK(tb.text == L"eta");

        CHECK(tb.undo());
        CHECK(tb.text == L"beta");
        CHECK(tb.undo());
        CHECK(tb.text == L"alpha beta");
        CHECK(tb.caret == 0);

        CHECK(tb.del(true)); // "alpha "
        CHECK(tb.text == L"beta");
        CHECK(tb.undo());
        CHECK(tb.text == L"alpha beta");
    }

    // Caret movement breaks undo grouping
    {
        TextBuffer tb;
        type_str(tb, L"ab");
        tb.move_left(false, false);
        tb.move_right(false, false);
        type_str(tb, L"cd");
        CHECK(tb.text == L"abcd");

        CHECK(tb.undo());
        CHECK(tb.text == L"ab");
        CHECK(tb.undo());
        CHECK(tb.text.empty());
    }

    // Paste / Cut / replace_range / no-op paste
    {
        TextBuffer tb;
        CHECK(tb.replace_selection(L"tag:fa"));
        CHECK(tb.replace_range(4, 2, L"fav "));
        CHECK(tb.text == L"tag:fav ");

        tb.select_all();
        CHECK(!tb.replace_selection(L"tag:fav ")); // identical text -> no-op undo entry
        CHECK(!tb.has_selection());

        CHECK(tb.undo());
        CHECK(tb.text == L"tag:fa");
        CHECK(tb.undo());
        CHECK(tb.text.empty());
    }

    // Undo capacity limit
    {
        TextBuffer tb;
        for (size_t i = 0; i < TextBuffer::MAX_UNDO + 20; ++i) {
            CHECK(tb.replace_all(std::to_wstring(i)));
        }
        size_t steps = 0;
        while (tb.undo()) ++steps;
        CHECK(steps == TextBuffer::MAX_UNDO);
    }
}

} // namespace

int main() {
    try {
        run_text_buffer_tests();
    } catch (...) {
        (void)std::fputs("text_buffer_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures != 0;
}
