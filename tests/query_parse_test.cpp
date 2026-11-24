#include "check.hpp"

#include <usyseek/ui/query.hpp>

#pragma comment(lib, "user32.lib")

#include <cstdio>
#include <string>
#include <vector>

using usyseek::ui::looks_like_path;
using usyseek::ui::parse_query;
using usyseek::ui::ParsedQuery;
using usyseek::ui::tag_key;

namespace {

void test_looks_like_path() {
    CHECK(looks_like_path(L"C:\\Photos"));
    CHECK(looks_like_path(L"c:/photos/2025"));
    CHECK(looks_like_path(L"D:"));
    CHECK(looks_like_path(L"D: tag:fav"));
    CHECK(looks_like_path(L"\\\\server\\share"));
    CHECK(looks_like_path(L"\\\\?\\C:\\Photos"));

    // Must not misclassify free text / non-ASCII / bare slashes as paths
    CHECK(!looks_like_path(L"x:men"));
    CHECK(!looks_like_path(L"a:b"));
    CHECK(!looks_like_path(L"c:sharp"));
    CHECK(!looks_like_path(L"t:fav"));
    CHECK(!looks_like_path(L"\u03B1:\\foo")); // Greek alpha
    CHECK(!looks_like_path(L"\\\\"));
    CHECK(!looks_like_path(L"\\\\ "));
    CHECK(!looks_like_path(L"\\\\\\foo"));
    CHECK(!looks_like_path(L""));
}

void test_basic_modifiers() {
    {
        const ParsedQuery q = parse_query(L"   ");
        CHECK(q.empty());
    }
    {
        const ParsedQuery q = parse_query(L"red circle");
        CHECK(q.text == L"red circle");
        CHECK(!q.empty());
    }
    {
        const ParsedQuery q = parse_query(L"tag:Fav -tag:WIP !tag:\"Work In Progress\" name:cat type:image sunset");
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
        CHECK(q.not_tags == (std::vector<std::string>{"wip", "work in progress"}));
        CHECK(q.names == (std::vector<std::string>{"cat"}));
        CHECK(q.kind.has_value() && *q.kind == 0);
        CHECK(!q.unknown_type.has_value());
        CHECK(q.text == L"sunset");
    }
    {
        const ParsedQuery q = parse_query(L"TYPE:VIDEOS");
        CHECK(q.kind.has_value() && *q.kind == 1);
        CHECK(!q.unknown_type.has_value());
    }
    {
        const ParsedQuery q = parse_query(L"type:\"  photo  \"");
        CHECK(q.kind.has_value() && *q.kind == 0);
        CHECK(!q.unknown_type.has_value());
    }
    {
        const ParsedQuery q = parse_query(L"type:audio");
        CHECK(!q.kind.has_value());
        CHECK(q.unknown_type.has_value() && *q.unknown_type == L"audio");
    }
}

void test_whitespace_and_empty_values() {
    {
        const ParsedQuery q = parse_query(L"tag: -tag: !tag: name: type: dir: in: similar:");
        CHECK(q.empty());
    }
    {
        const ParsedQuery q = parse_query(L"tag:\"   \" -tag:\"\" name:\"  \" type:\"  \" dir:\"  \" similar:\"  \"");
        CHECK(q.empty());
    }
    {
        const ParsedQuery q = parse_query(L"\"\" foo \"\" bar");
        CHECK(q.text == L"foo bar");
    }
    {
        // Free text containing x:men must stay free text, not become a dir filter
        const ParsedQuery q = parse_query(L"x:men poster");
        CHECK(q.dirs.empty());
        CHECK(q.text == L"x:men poster");
    }
    {
        CHECK(tag_key(L"  My Tag  ") == "my tag");
        CHECK(tag_key(L"   ").empty());
    }
}

void test_dir_and_bare_path_parsing() {
    // Unquoted dir with spaces terminated by ANY modifier (case-insensitive, spaces or tabs)
    {
        const ParsedQuery q = parse_query(L"dir:C:\\My Photos\\2025 TYPE:image !TAG:blurry");
        CHECK(q.dirs == (std::vector<std::string>{"c:\\my photos\\2025"}));
        CHECK(q.kind.has_value() && *q.kind == 0);
        CHECK(q.not_tags == (std::vector<std::string>{"blurry"}));
    }
    {
        const ParsedQuery q = parse_query(L"in:C:/My Photos\ttag:fav dir:D:\\Archive\\");
        CHECK(q.dirs == (std::vector<std::string>{"c:\\my photos", "d:\\archive"}));
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
    }
    {
        const ParsedQuery q = parse_query(L"dir:\"C:\\My Photos\" sunset type:video");
        CHECK(q.dirs == (std::vector<std::string>{"c:\\my photos"}));
        CHECK(q.text == L"sunset");
        CHECK(q.kind.has_value() && *q.kind == 1);
    }
    {
        // Bare path followed by trailing modifiers
        const ParsedQuery q = parse_query(L"C:\\My Photos\\Trips tag:fav -tag:raw type:image");
        CHECK(q.dirs == (std::vector<std::string>{"c:\\my photos\\trips"}));
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
        CHECK(q.not_tags == (std::vector<std::string>{"raw"}));
        CHECK(q.kind.has_value() && *q.kind == 0);
    }
    {
        // Bare drive root followed by modifier
        const ParsedQuery q = parse_query(L"C:\\ tag:fav");
        CHECK(q.dirs == (std::vector<std::string>{"c:"}));
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
    }
    {
        // Empty dir: followed by another modifier must not swallow it
        const ParsedQuery q = parse_query(L"dir: tag:fav");
        CHECK(q.dirs.empty());
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
    }
    {
        // Path segment containing modifier-like substring without leading whitespace is preserved
        const ParsedQuery q = parse_query(L"dir:C:\\Photos\\tag:set tag:fav");
        CHECK(q.dirs == (std::vector<std::string>{"c:\\photos\\tag:set"}));
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
    }
}

void test_similar_parsing() {
    {
        const ParsedQuery q = parse_query(L"similar:C:\\My Photos\\cat.jpg");
        CHECK(q.similar.has_value() && *q.similar == L"C:\\My Photos\\cat.jpg");
    }
    {
        const ParsedQuery q = parse_query(L"similar:\"C:/My Photos/cat.jpg\" tag:fav type:image");
        CHECK(q.similar.has_value() && *q.similar == L"C:\\My Photos\\cat.jpg");
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
        CHECK(q.kind.has_value() && *q.kind == 0);
    }
    {
        const ParsedQuery q = parse_query(L"similar:C:\\My Photos\\cat.jpg -tag:wip dir:D:\\Keep");
        CHECK(q.similar.has_value() && *q.similar == L"C:\\My Photos\\cat.jpg");
        CHECK(q.not_tags == (std::vector<std::string>{"wip"}));
        CHECK(q.dirs == (std::vector<std::string>{"d:\\keep"}));
    }
    {
        const ParsedQuery q = parse_query(L"similar:\\\\?\\C:\\Photos\\cat.jpg");
        CHECK(q.similar.has_value() && *q.similar == L"C:\\Photos\\cat.jpg");
    }
    {
        const ParsedQuery q = parse_query(L"similar: tag:fav");
        CHECK(!q.similar.has_value());
        CHECK(q.tags == (std::vector<std::string>{"fav"}));
    }
}

} // namespace

int main() {
    try {
        test_looks_like_path();
        test_basic_modifiers();
        test_whitespace_and_empty_values();
        test_dir_and_bare_path_parsing();
        test_similar_parsing();
    } catch (...) {
        (void)std::fputs("query_parse_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures != 0;
}
