#include "check.hpp"
#include "usyseek/path_rules.hpp"

using usyseek::is_duplicate_path_rule;
using usyseek::normalize_path_rule;

int main() {
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
    CHECK(!is_duplicate_path_rule(L"D:\\Photo", {L"D:\\Photos"}));
    CHECK(!is_duplicate_path_rule(L"D:\\Photos", {L"D:\\Photo"}));
    CHECK(!is_duplicate_path_rule(L"*\\cache", list));
    CHECK(!is_duplicate_path_rule(L"D:\\Media\\Holiday", {}));

    const std::vector<std::wstring> included{L"D:\\Media\\Holiday", L"E:\\Backup"};
    CHECK(is_duplicate_path_rule(L"d:/media/holiday/", included));
    CHECK(is_duplicate_path_rule(L"E:\\BACKUP", included));
    CHECK(!is_duplicate_path_rule(L"D:\\Media\\Holiday\\2024", included));
    CHECK(!is_duplicate_path_rule(L"E:\\Backups", included));

    return usyseek::test::failures != 0;
}
