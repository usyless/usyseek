#pragma once

#include <usyseek/string_helpers.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace usyseek::ui {

// ---------------------------------------------------------------------------------------------
// Query language
//   free text          semantic search
//   tag:x  -tag:x      require / exclude a tag ("quoted values" allowed)
//   similar:<path>     visually similar to an indexed file
//   name:x             file name contains (prefix tokens)
//   dir:<path> / a bare path   restrict to a folder (recursive)
//   type:image|video
// ---------------------------------------------------------------------------------------------

struct ParsedQuery {
    std::wstring text;
    std::vector<std::string> tags;
    std::vector<std::string> not_tags;
    std::vector<std::string> dirs;
    std::vector<std::string> names;
    std::optional<std::wstring> similar;
    std::optional<int> kind;
    std::optional<std::wstring> unknown_type;

    [[nodiscard]] bool empty() const noexcept {
        return text.empty() && tags.empty() && not_tags.empty() && dirs.empty() && names.empty() && !similar && !kind && !unknown_type;
    }
};

namespace detail {

[[nodiscard]] inline bool is_wspace(wchar_t c) noexcept {
    return std::iswspace(static_cast<wint_t>(c)) != 0;
}

[[nodiscard]] constexpr bool is_ascii_alpha(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

[[nodiscard]] constexpr wchar_t ascii_lower(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

[[nodiscard]] inline bool starts_with_ascii_ci(std::wstring_view s, std::wstring_view prefix) noexcept {
    return s.size() >= prefix.size() &&
           std::ranges::equal(s.substr(0, prefix.size()), prefix, [](wchar_t a, wchar_t b) noexcept {
               return ascii_lower(a) == ascii_lower(b);
           });
}

[[nodiscard]] inline std::wstring_view trim_wspace(std::wstring_view s) noexcept {
    while (!s.empty() && is_wspace(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_wspace(s.back())) s.remove_suffix(1);
    return s;
}

[[nodiscard]] inline std::wstring lower_wide(std::wstring_view s) {
    std::wstring out{s};
    size_t offset = 0;
    constexpr auto MAX_CHUNK = static_cast<size_t>(std::numeric_limits<DWORD>::max());
    while (offset < out.size()) {
        const auto chunk = static_cast<DWORD>(std::min(out.size() - offset, MAX_CHUNK));
        CharLowerBuffW(out.data() + offset, chunk);
        offset += chunk;
    }
    return out;
}

[[nodiscard]] inline std::wstring normalise_dir_wide(std::wstring_view d) {
    std::wstring s = lower_wide(trim_wspace(d));
    std::ranges::replace(s, L'/', L'\\');
    if (s.starts_with(L"\\\\?\\")) s.erase(0, 4);
    while (s.size() > 2 && s.back() == L'\\') s.pop_back();
    if (s.size() == 3 && s[1] == L':' && s[2] == L'\\') s.pop_back();
    return s;
}

[[nodiscard]] inline bool is_modifier_prefix(std::wstring_view s) noexcept {
    static constexpr std::array<std::wstring_view, 8> MODIFIERS = {
        L"similar:", L"-tag:", L"!tag:", L"tag:", L"name:", L"type:", L"dir:", L"in:",
    };
    return std::ranges::any_of(MODIFIERS, [s](std::wstring_view m) noexcept {
        return starts_with_ascii_ci(s, m);
    });
}

} // namespace detail

[[nodiscard]] inline bool looks_like_path(std::wstring_view s) noexcept {
    if (s.size() >= 2 && s[1] == L':' && detail::is_ascii_alpha(s[0])) {
        return s.size() == 2 || s[2] == L'\\' || s[2] == L'/' || detail::is_wspace(s[2]);
    }
    return s.size() >= 3 && s[0] == L'\\' && s[1] == L'\\' && s[2] != L'\\' && s[2] != L'/' && !detail::is_wspace(s[2]);
}

[[nodiscard]] inline std::string tag_key(std::wstring_view t) {
    return to_utf8(detail::lower_wide(detail::trim_wspace(t)));
}

[[nodiscard]] inline ParsedQuery parse_query(std::wstring_view q) {
    ParsedQuery out;
    size_t i = 0;

    const auto read_value = [&](size_t from) noexcept -> std::wstring_view {
        if (from < q.size() && q[from] == L'"') {
            const size_t end = q.find(L'"', from + 1);
            const size_t len = (end == std::wstring_view::npos ? q.size() : end) - from - 1;
            i = end == std::wstring_view::npos ? q.size() : end + 1;
            return q.substr(from + 1, len);
        }
        size_t end = from;
        while (end < q.size() && !detail::is_wspace(q[end])) ++end;
        i = end;
        return q.substr(from, end - from);
    };

    const auto read_path_value = [&](size_t from) noexcept -> std::wstring_view {
        if (from < q.size() && q[from] == L'"') return read_value(from);
        size_t pos = from;
        while (pos < q.size()) {
            if (detail::is_wspace(q[pos])) {
                size_t next = pos;
                while (next < q.size() && detail::is_wspace(q[next])) ++next;
                if (next < q.size() && detail::is_modifier_prefix(q.substr(next))) {
                    const std::wstring_view v = q.substr(from, pos - from);
                    i = next;
                    return v;
                }
                pos = next;
            } else {
                ++pos;
            }
        }
        i = q.size();
        return q.substr(from);
    };

    while (i < q.size()) {
        while (i < q.size() && detail::is_wspace(q[i])) ++i;
        if (i >= q.size()) break;
        const std::wstring_view rest = q.substr(i);
        const auto starts = [rest](std::wstring_view p) noexcept {
            return detail::starts_with_ascii_ci(rest, p);
        };

        if (starts(L"similar:")) {
            if (const auto raw = detail::trim_wspace(read_path_value(i + 8)); !raw.empty()) {
                std::wstring v{raw};
                std::ranges::replace(v, L'/', L'\\');
                if (v.starts_with(L"\\\\?\\")) v.erase(0, 4);
                if (!v.empty()) out.similar = std::move(v);
            }
            continue;
        }
        if (starts(L"-tag:") || starts(L"!tag:")) {
            if (auto key = tag_key(read_value(i + 5)); !key.empty()) out.not_tags.push_back(std::move(key));
            continue;
        }
        if (starts(L"tag:")) {
            if (auto key = tag_key(read_value(i + 4)); !key.empty()) out.tags.push_back(std::move(key));
            continue;
        }
        if (starts(L"name:")) {
            if (const auto v = detail::trim_wspace(read_value(i + 5)); !v.empty()) out.names.push_back(to_utf8(v));
            continue;
        }
        if (starts(L"type:")) {
            const std::wstring v = detail::lower_wide(detail::trim_wspace(read_value(i + 5)));
            if (v.starts_with(L"im") || v.starts_with(L"ph") || v.starts_with(L"pic")) {
                out.kind = 0;
            } else if (v.starts_with(L"vid")) {
                out.kind = 1;
            } else if (!v.empty()) {
                out.unknown_type = v;
            }
            continue;
        }
        if (starts(L"dir:") || starts(L"in:")) {
            const size_t off = starts(L"dir:") ? 4 : 3;
            // Paths often contain spaces: take everything up to the next modifier unless quoted
            if (const auto d = detail::normalise_dir_wide(read_path_value(i + off)); !d.empty()) {
                out.dirs.push_back(to_utf8(d));
            }
            continue;
        }
        if (looks_like_path(rest)) {
            if (const auto d = detail::normalise_dir_wide(read_path_value(i)); !d.empty()) {
                out.dirs.push_back(to_utf8(d));
            }
            continue;
        }
        if (const auto w = detail::trim_wspace(read_value(i)); !w.empty()) {
            if (!out.text.empty()) out.text.push_back(L' ');
            out.text.append(w);
        }
    }
    return out;
}

} // namespace usyseek::ui
