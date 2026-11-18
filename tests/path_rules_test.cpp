#include "check.hpp"
#include "usyseek/path_rules.hpp"

#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

using usyseek::is_duplicate_path_rule;
using usyseek::normalize_path_rule;

namespace {

void run_path_rules_tests() {
    const std::vector<std::wstring> list{L"D:\\Media\\Holiday", L"*\\node_modules", L"C:\\"};

    CHECK(is_duplicate_path_rule(L"d:\\media\\HOLIDAY", list));
    CHECK(is_duplicate_path_rule(L"D:\\Media\\Holiday\\", list));
    CHECK(is_duplicate_path_rule(L"D:/Media/Holiday", list));
    CHECK(is_duplicate_path_rule(L"D:\\\\Media\\\\Holiday", list));
    CHECK(is_duplicate_path_rule(L"  D:\\Media\\Holiday  ", list));
    CHECK(is_duplicate_path_rule(L"*\\Node_Modules\\", list));
    CHECK(is_duplicate_path_rule(L"*\\Node_Modules\\*", list));
    CHECK(is_duplicate_path_rule(L"*/node_modules", list));
    CHECK(is_duplicate_path_rule(L"*/node_modules/*", list));
    CHECK(is_duplicate_path_rule(L"c:", list));
    CHECK(is_duplicate_path_rule(L"C:/", list));
    CHECK(is_duplicate_path_rule(L"C:\\*", list));
    CHECK(is_duplicate_path_rule(L"C:/*", list));
    CHECK(is_duplicate_path_rule(L"D:\\Media\\Holiday\\*", list));

    CHECK(normalize_path_rule(L"C:\\") == L"c:\\");
    CHECK(normalize_path_rule(L"C:\\\\") == L"c:\\");
    CHECK(normalize_path_rule(L"C:/") == L"c:\\");
    CHECK(normalize_path_rule(L"C:\\*") == L"c:\\");
    CHECK(normalize_path_rule(L"C:/*") == L"c:\\");
    CHECK(normalize_path_rule(L"C:/Temp/") == L"c:\\temp");
    CHECK(normalize_path_rule(L"C:/Temp/*") == L"c:\\temp");

    CHECK(!is_duplicate_path_rule(L"D:\\Media", list));
    CHECK(!is_duplicate_path_rule(L"D:\\Media\\Holiday\\2024", list));
    CHECK(!is_duplicate_path_rule(L"D:\\Photo", std::array<std::wstring_view, 1>{L"D:\\Photos"}));
    CHECK(!is_duplicate_path_rule(L"D:\\Photos", std::array<std::wstring_view, 1>{L"D:\\Photo"}));
    CHECK(!is_duplicate_path_rule(L"*\\cache", list));
    CHECK(!is_duplicate_path_rule(L"D:\\Media\\Holiday", std::array<std::wstring_view, 0>{}));

    const std::vector<std::wstring> included{L"D:\\Media\\Holiday", L"E:\\Backup"};
    CHECK(is_duplicate_path_rule(L"d:/media/holiday/", included));
    CHECK(is_duplicate_path_rule(L"E:\\BACKUP", included));
    CHECK(!is_duplicate_path_rule(L"D:\\Media\\Holiday\\2024", included));
    CHECK(!is_duplicate_path_rule(L"E:\\Backups", included));
}

} // namespace

int main() {
    try {
        run_path_rules_tests();
    } catch (...) {
        (void)std::fputs("path_rules_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures != 0;
}
