#include "check.hpp"
#include "usyseek/exclusions.hpp"

using usyseek::is_duplicate_exclusion;
using usyseek::normalize_exclusion;

int main() {
    const std::vector<std::wstring> list{L"D:\\Media\\Holiday", L"*\\node_modules", L"C:\\"};

    CHECK(is_duplicate_exclusion(L"d:\\media\\HOLIDAY", list));
    CHECK(is_duplicate_exclusion(L"D:\\Media\\Holiday\\", list));
    CHECK(is_duplicate_exclusion(L"D:/Media/Holiday", list));
    CHECK(is_duplicate_exclusion(L"D:\\\\Media\\\\Holiday", list));
    CHECK(is_duplicate_exclusion(L"  D:\\Media\\Holiday  ", list));
    CHECK(is_duplicate_exclusion(L"*\\Node_Modules\\", list));
    CHECK(is_duplicate_exclusion(L"*/node_modules", list));
    CHECK(is_duplicate_exclusion(L"c:", list));

    CHECK(normalize_exclusion(L"C:\\") == L"c:\\");
    CHECK(normalize_exclusion(L"C:\\\\") == L"c:\\");
    CHECK(normalize_exclusion(L"C:/Temp/") == L"c:\\temp");

    CHECK(!is_duplicate_exclusion(L"D:\\Media", list));
    CHECK(!is_duplicate_exclusion(L"D:\\Media\\Holiday\\2024", list));
    CHECK(!is_duplicate_exclusion(L"D:\\Photo", {L"D:\\Photos"}));
    CHECK(!is_duplicate_exclusion(L"D:\\Photos", {L"D:\\Photo"}));
    CHECK(!is_duplicate_exclusion(L"*\\cache", list));
    CHECK(!is_duplicate_exclusion(L"D:\\Media\\Holiday", {}));

    return usyseek::test::failures != 0;
}
