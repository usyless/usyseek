#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace usyseek {

inline std::wstring normalize_path_rule(std::wstring_view raw) {
    const auto is_space = [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)) != 0; };
    size_t b = 0, e = raw.size();
    while (b < e && is_space(raw[b])) ++b;
    while (e > b && is_space(raw[e - 1])) --e;

    std::wstring out;
    for (size_t i = b; i < e; ++i) {
        const wchar_t c = raw[i] == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(static_cast<wint_t>(raw[i])));
        if (c == L'\\' && !out.empty() && out.back() == L'\\') continue;
        out.push_back(c);
    }

    const bool drive_root = out.size() == 3 && out[1] == L':' && out[2] == L'\\';
    if (!drive_root && !out.empty() && out.back() == L'\\') out.pop_back();
    if (out.size() == 2 && out[1] == L':') out.push_back(L'\\');
    return out;
}

inline bool is_duplicate_path_rule(std::wstring_view candidate, const std::vector<std::wstring>& existing) {
    const std::wstring wanted = normalize_path_rule(candidate);
    return std::any_of(existing.begin(), existing.end(), [&](const std::wstring& e) { return normalize_path_rule(e) == wanted; });
}

} // namespace usyseek
