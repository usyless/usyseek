#include "check.hpp"
#include "usyseek/string_helpers.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

// Concept checks
static_assert(usyseek::detail::StringPart<char>);
static_assert(usyseek::detail::StringPart<wchar_t>);
static_assert(usyseek::detail::StringPart<char8_t>);
static_assert(usyseek::detail::StringPart<char16_t>);
static_assert(usyseek::detail::StringPart<char32_t>);
static_assert(usyseek::detail::StringPart<const char*>);
static_assert(usyseek::detail::StringPart<const wchar_t*>);
static_assert(usyseek::detail::StringPart<decltype("literal")>);
static_assert(usyseek::detail::StringPart<decltype(L"literal")>);
static_assert(usyseek::detail::StringPart<std::string>);
static_assert(usyseek::detail::StringPart<std::wstring>);
static_assert(usyseek::detail::StringPart<std::string_view>);
static_assert(usyseek::detail::StringPart<std::wstring_view>);
static_assert(usyseek::detail::StringPart<std::u8string_view>);
static_assert(usyseek::detail::StringPart<std::u16string_view>);
static_assert(usyseek::detail::StringPart<std::u32string_view>);
static_assert(usyseek::detail::StringPart<usyseek::FixedString<16>>);
static_assert(usyseek::detail::StringPart<usyseek::FixedWString<16>>);
static_assert(usyseek::detail::StringPart<std::filesystem::path>);

static_assert(!usyseek::detail::StringPart<int>);
static_assert(!usyseek::detail::StringPart<double>);
static_assert(!usyseek::detail::StringPart<bool>);
static_assert(!usyseek::detail::StringPart<std::nullptr_t>);

// Compile-time constexpr verification
consteval bool constexpr_roundtrip_check() {
    std::string s = "start:";
    usyseek::append_to(s, L"caf\u00E9", '-', u8"\u65E5\u672C", u"\U0001F600", U'!');
    if (s != "start:caf\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x98\x80!") return false;

    std::wstring w;
    usyseek::overwrite_to(w, s);
    if (w != L"start:caf\u00E9-\u65E5\u672C\U0001F600!") return false;

    usyseek::overwrite_to(w, L"[", std::wstring_view{w}.substr(6, 4), L"]");
    if (w != L"[caf\u00E9]") return false;

    usyseek::FixedWString<32> fw{L"ok:"};
    fw.append("abc", L'/', u"\u2022");
    if (fw != L"ok:abc/\u2022") return false;

    fw.assign("new:", fw);
    return fw == L"new:ok:abc/\u2022";
}
static_assert(constexpr_roundtrip_check());

void test_mixed_append_and_concat() {
    const std::string utf8_str = "hello";
    const std::wstring wide_str = L"w\u00F6rld";
    const char* null_c = nullptr;
    const wchar_t* null_w = nullptr;
    const std::filesystem::path path = L"C:\\dir\\caf\u00E9.png";
    const auto fixed_n = usyseek::to_fixed_str(42);
    const auto fixed_w = usyseek::to_fixed_wstr(99);

    std::string out = "pre:";
    usyseek::append_to(
        out,
        utf8_str, ' ', wide_str, L' ',
        null_c, null_w,
        fixed_n, '/', fixed_w, " ",
        u8"\u65E5\u672C", " ",
        u"\U0001F600", " ",
        U"\U0001D11E", " ",
        path
    );
    CHECK(out == "pre:hello w\xC3\xB6rld 42/99 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80 \xF0\x9D\x84\x9E C:\\dir\\caf\xC3\xA9.png");

    const std::wstring wout = usyseek::concat_wide(
        "pre:", utf8_str, ' ', wide_str, L' ',
        null_c, null_w,
        fixed_n, L'/', fixed_w, L" ",
        u8"\u65E5\u672C", L" ",
        u"\U0001F600", L" ",
        U"\U0001D11E", L" ",
        path
    );
    CHECK(wout == L"pre:hello w\u00F6rld 42/99 \u65E5\u672C \U0001F600 \U0001D11E C:\\dir\\caf\u00E9.png");

    // Roundtrip check
    CHECK(usyseek::to_utf8(wout) == out);
    CHECK(usyseek::to_wide(out) == wout);
}

void test_overwrite_and_capacity_reuse() {
    std::wstring dst;
    dst.reserve(128);
    const wchar_t* const initial_buf = dst.data();

    usyseek::overwrite_to(dst, "initial ", L"value ", usyseek::to_fixed_str(123));
    CHECK(dst == L"initial value 123");
    CHECK(dst.data() == initial_buf);

    usyseek::overwrite_to(dst, "shorter");
    CHECK(dst == L"shorter");
    CHECK(dst.data() == initial_buf);

    usyseek::overwrite_to(dst);
    CHECK(dst.empty());

    usyseek::overwrite_to(dst, "", L"");
    CHECK(dst.empty());
}

void test_self_aliasing() {
    // append_to self-alias forcing reallocation
    std::string s = "abc";
    usyseek::append_to(s, s, L"-", s, s, s, s, s, s, s, s);
    CHECK(s == "abcabc-abcabcabcabcabcabcabcabc");

    // append_to substring self-alias
    std::wstring w = L"hello world";
    usyseek::append_to(w, " / ", std::wstring_view{w}.substr(6, 5), " / ", std::wstring_view{w}.substr(0, 5));
    CHECK(w == L"hello world / world / hello");

    // overwrite_to self-alias growing
    usyseek::overwrite_to(w, "[prefix:", std::wstring_view{w}.substr(0, 5), ":suffix]");
    CHECK(w == L"[prefix:hello:suffix]");

    // overwrite_to self-alias shrinking from high offset
    std::string big(200, 'x');
    big.replace(150, 5, "HELLO");
    usyseek::overwrite_to(big, std::string_view{big}.substr(150, 5), L"!");
    CHECK(big == "HELLO!");
}

void test_unicode_edge_cases() {
    // Embedded NULs preserved
    const std::string_view with_nul{"a\0b", 3};
    const std::wstring w_nul = usyseek::to_wide(with_nul);
    CHECK(w_nul.size() == 3);
    CHECK(w_nul[0] == L'a' && w_nul[1] == L'\0' && w_nul[2] == L'b');
    const std::string back_nul = usyseek::to_utf8(w_nul);
    CHECK(back_nul.size() == 3);
    CHECK(back_nul[0] == 'a' && back_nul[1] == '\0' && back_nul[2] == 'b');

    // Boundary codepoints: U+007F, U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF, U+10000, U+10FFFF
    const std::u32string boundaries = {
        U'\x007F', U'\x0080', U'\x07FF', U'\x0800', U'\xD7FF', U'\xE000', U'\xFFFF',
        static_cast<char32_t>(0x10000u), static_cast<char32_t>(0x10FFFFu),
    };
    const std::string u8 = usyseek::concat_utf8(boundaries);
    const std::wstring w = usyseek::concat_wide(u8);
    const std::u32string u32_back = usyseek::concat_to<char32_t>(w);
    CHECK(u32_back == boundaries);
    CHECK(usyseek::concat_utf8(w) == u8);

    // Ill-formed UTF-8 sequences -> U+FFFD maximal subpart
    // Overlong '/' (0xC0 0xAF) -> 2 replacements
    CHECK(usyseek::to_wide(std::string_view{"\xC0\xAF", 2}) == L"\uFFFD\uFFFD");
    // Truncated 3-byte sequence (0xE2 0x82) followed by 'A' -> 1 replacement + 'A'
    CHECK(usyseek::to_wide(std::string_view{"\xE2\x82" "A", 3}) == L"\uFFFD" L"A");
    // Truncated 4-byte sequence (0xF0 0x9F 0x98) -> 1 replacement
    CHECK(usyseek::to_wide(std::string_view{"\xF0\x9F\x98", 3}) == L"\uFFFD");
    // UTF-8 encoded surrogate U+D800 (0xED 0xA0 0x80) -> 3 replacements
    CHECK(usyseek::to_wide(std::string_view{"\xED\xA0\x80", 3}) == L"\uFFFD\uFFFD\uFFFD");
    // Out-of-range > U+10FFFF (0xF4 0x90 0x80 0x80) -> 4 replacements
    CHECK(usyseek::to_wide(std::string_view{"\xF4\x90\x80\x80", 4}) == L"\uFFFD\uFFFD\uFFFD\uFFFD");

    // Unpaired UTF-16 surrogates -> U+FFFD (\xEF\xBF\xBD)
    const std::array<char16_t, 4> bad_u16 = {0xD83Du, u'a', 0xDE00u, 0xD800u};
    const std::string bad_u8 = usyseek::concat_utf8(std::u16string_view{bad_u16.data(), bad_u16.size()});
    CHECK(bad_u8 == "\xEF\xBF\xBD" "a\xEF\xBF\xBD\xEF\xBF\xBD");
}

void test_fixed_string_mixed_and_truncation() {
    usyseek::FixedString<6> fs;
    // Capacity is 5 bytes. 'a' (1) + '😀' (4) = 5 bytes -> fits completely
    usyseek::append_to(fs, L"a\U0001F600");
    CHECK(fs == "a\xF0\x9F\x98\x80");

    // Capacity is 5 bytes. "ab" (2) + '😀' (4) = 6 > 5 -> must stop before splitting '😀'
    usyseek::overwrite_to(fs, L"ab\U0001F600");
    CHECK(fs == "ab");

    // Wide fixed string: capacity 2 wchar_t. "a" (1) + '😀' (2 UTF-16 units) -> stops before lone high surrogate
    usyseek::FixedWString<3> fw;
    usyseek::overwrite_to(fw, "a\xF0\x9F\x98\x80");
    if constexpr (sizeof(wchar_t) == 2) {
        CHECK(fw == L"a");
    } else {
        CHECK(fw == L"a\U0001F600");
    }

    // Self-alias on FixedBasicString
    usyseek::FixedWString<32> self{L"hi"};
    self.append(L":", self, "!");
    CHECK(self == L"hi:hi!");
    self.assign("[", self, "]");
    CHECK(self == L"[hi:hi!]");
}

} // namespace

int main() {
    try {
        test_mixed_append_and_concat();
        test_overwrite_and_capacity_reuse();
        test_self_aliasing();
        test_unicode_edge_cases();
        test_fixed_string_mixed_and_truncation();
    } catch (...) {
        (void)std::fputs("string_helpers_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures != 0;
}
