#pragma once

#include <algorithm>
#include <concepts>
#include <cwctype>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace usyseek {

inline std::wstring normalize_path_rule(std::wstring_view raw) {
    const auto is_space = [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)) != 0; };
    size_t b = 0, e = raw.size();
    while (b < e && is_space(raw[b])) ++b;
    while (e > b && is_space(raw[e - 1])) --e;

    std::wstring out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) {
        const wchar_t c = raw[i] == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(static_cast<wint_t>(raw[i])));
        if (c == L'\\' && !out.empty() && out.back() == L'\\') continue;
        out.push_back(c);
    }

    while (!out.empty() && (out.back() == L'*' || out.back() == L'\\')) {
        if (out.size() == 3 && out[1] == L':' && out[2] == L'\\') break;
        out.pop_back();
    }
    if (out.size() == 2 && out[1] == L':') out.push_back(L'\\');
    return out;
}

template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, std::wstring_view>
inline bool is_duplicate_path_rule(std::wstring_view candidate, const R& existing) {
    const std::wstring wanted = normalize_path_rule(candidate);
    return std::ranges::any_of(existing, [&](std::wstring_view e) { return normalize_path_rule(e) == wanted; });
}

} // namespace usyseek
