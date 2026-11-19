#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace usyseek {

/// Fixed-capacity stack string with implicit `std::basic_string_view` conversion and zero heap allocations.
template <typename Char, size_t N>
struct FixedBasicString {
    static_assert(N > 0);
    std::array<Char, N> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t len = 0;

    constexpr FixedBasicString() noexcept {
        if consteval { buf.fill(Char{}); }
        buf[0] = Char{};
    }
    constexpr FixedBasicString(const FixedBasicString& other) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(other.view());
    }
    constexpr FixedBasicString& operator=(const FixedBasicString& other) noexcept {
        if (this != &other) assign(other.view());
        return *this;
    }
    constexpr FixedBasicString(FixedBasicString&& other) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(other.view());
    }
    constexpr FixedBasicString& operator=(FixedBasicString&& other) noexcept {
        if (this != &other) assign(other.view());
        return *this;
    }
    constexpr ~FixedBasicString() noexcept = default;
    constexpr FixedBasicString(std::basic_string_view<Char> sv) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(sv);
    }
    constexpr FixedBasicString(const Char* s) noexcept
        : FixedBasicString(s ? std::basic_string_view<Char>{s} : std::basic_string_view<Char>{}) {}
    template <size_t M>
    constexpr FixedBasicString(const FixedBasicString<Char, M>& other) noexcept {
        if consteval { buf.fill(Char{}); }
        assign(other.view());
    }

    constexpr void clear() noexcept {
        len = 0;
        buf[0] = Char{};
    }

    constexpr void assign(std::basic_string_view<Char> sv) noexcept {
        const size_t n = std::min(sv.size(), N - 1);
        for (size_t i = 0; i < n; ++i) buf[i] = sv[i];
        buf[n] = Char{};
        len = n;
    }

    constexpr void push_back(Char c) noexcept {
        if (len + 1 < N) {
            buf[len++] = c;
            buf[len] = Char{};
        }
    }

    constexpr void append(std::basic_string_view<Char> sv) noexcept {
        const size_t room = (N - 1) - len;
        const size_t n = std::min(sv.size(), room);
        for (size_t i = 0; i < n; ++i) buf[len + i] = sv[i];
        len += n;
        buf[len] = Char{};
    }

    [[nodiscard]] constexpr const Char* data() const noexcept { return buf.data(); }
    [[nodiscard]] constexpr Char* data() noexcept { return buf.data(); }
    [[nodiscard]] constexpr const Char* c_str() const noexcept { return buf.data(); }
    [[nodiscard]] constexpr size_t size() const noexcept { return len; }
    [[nodiscard]] constexpr size_t length() const noexcept { return len; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return N - 1; }
    [[nodiscard]] constexpr bool empty() const noexcept { return len == 0; }
    [[nodiscard]] constexpr const Char* begin() const noexcept { return buf.data(); }
    [[nodiscard]] constexpr const Char* end() const noexcept { return buf.data() + len; }
    [[nodiscard]] constexpr Char operator[](size_t i) const noexcept { return buf[i]; }
    [[nodiscard]] constexpr std::basic_string_view<Char> view() const noexcept { return {buf.data(), len}; }
    [[nodiscard]] constexpr operator std::basic_string_view<Char>() const noexcept { return view(); }
    [[nodiscard]] std::basic_string<Char> str() const { return std::basic_string<Char>{buf.data(), len}; }

    friend constexpr bool operator==(const FixedBasicString& a, std::basic_string_view<Char> b) noexcept {
        return a.view() == b;
    }
    friend constexpr auto operator<=>(const FixedBasicString& a, std::basic_string_view<Char> b) noexcept {
        return a.view() <=> b;
    }
};

template <size_t N>
using FixedString = FixedBasicString<char, N>;

template <size_t N>
using FixedWString = FixedBasicString<wchar_t, N>;

/// Appends `parts...` onto `dst` in one capacity check without creating intermediate strings.
template <typename Char, typename... Ts>
constexpr void append_to(std::basic_string<Char>& dst, const Ts&... parts) {
    if constexpr (sizeof...(Ts) == 0) {
        return;
    } else {
        const std::array<std::basic_string_view<Char>, sizeof...(Ts)> views{std::basic_string_view<Char>{parts}...};
        size_t add = 0;
        for (const auto sv : views) add += sv.size();
        if (add == 0) return;

        const size_t old = dst.size();
        if consteval {
            std::basic_string<Char> tmp;
            tmp.reserve(old + add);
            tmp.append(dst);
            for (const auto sv : views) tmp.append(sv);
            dst = std::move(tmp);
            return;
        }

        // Snapshot views that alias `dst` before resize_and_overwrite may reallocate `dst`'s buffer.
        const Char* const old_begin = dst.data();
        const Char* const old_end = old_begin + old;
        std::array<bool, sizeof...(Ts)> self_alias{};
        std::array<size_t, sizeof...(Ts)> self_off{};
        for (size_t i = 0; i < views.size(); ++i) {
            const Char* p = views[i].data();
            if (!views[i].empty() && !std::less<const Char*>{}(p, old_begin) && std::less<const Char*>{}(p, old_end)) {
                self_alias[i] = true;
                self_off[i] = static_cast<size_t>(p - old_begin);
            }
        }

        dst.resize_and_overwrite(old + add, [&](Char* p, size_t) noexcept {
            Char* cur = p + old;
            for (size_t i = 0; i < views.size(); ++i) {
                if (!views[i].empty()) {
                    const Char* src = self_alias[i] ? (p + self_off[i]) : views[i].data();
                    std::char_traits<Char>::copy(cur, src, views[i].size());
                    cur += views[i].size();
                }
            }
            return old + add;
        });
    }
}

template <size_t N = 32, typename T>
    requires(std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>)
[[nodiscard]] inline FixedString<N> to_fixed_str(T val) noexcept {
    FixedString<N> out;
    const auto [ptr, ec] = std::to_chars(out.buf.data(), out.buf.data() + out.buf.size() - 1, val);
    out.len = (ec == std::errc{}) ? static_cast<size_t>(ptr - out.buf.data()) : 0u;
    out.buf[out.len] = '\0';
    return out;
}

template <size_t N = 48, std::floating_point T>
[[nodiscard]] inline FixedString<N> to_fixed_str(T val, int precision) noexcept {
    FixedString<N> out;
    const auto [ptr, ec] = std::to_chars(out.buf.data(), out.buf.data() + out.buf.size() - 1, val, std::chars_format::fixed, precision);
    out.len = (ec == std::errc{}) ? static_cast<size_t>(ptr - out.buf.data()) : 0u;
    out.buf[out.len] = '\0';
    return out;
}

template <size_t N = 32, typename T>
    requires(std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>)
[[nodiscard]] inline FixedWString<N> to_fixed_wstr(T val) noexcept {
    std::array<char, N> tmp; // NOLINT(cppcoreguidelines-pro-type-member-init)
    const auto [ptr, ec] = std::to_chars(tmp.data(), tmp.data() + tmp.size() - 1, val);
    FixedWString<N> out;
    if (ec == std::errc{}) {
        const auto n = static_cast<size_t>(ptr - tmp.data());
        for (size_t i = 0; i < n; ++i) out.buf[i] = static_cast<wchar_t>(tmp[i]);
        out.len = n;
        out.buf[n] = L'\0';
    }
    return out;
}

template <size_t N = 48, std::floating_point T>
[[nodiscard]] inline FixedWString<N> to_fixed_wstr(T val, int precision) noexcept {
    std::array<char, N> tmp; // NOLINT(cppcoreguidelines-pro-type-member-init)
    const auto [ptr, ec] = std::to_chars(tmp.data(), tmp.data() + tmp.size() - 1, val, std::chars_format::fixed, precision);
    FixedWString<N> out;
    if (ec == std::errc{}) {
        const auto n = static_cast<size_t>(ptr - tmp.data());
        for (size_t i = 0; i < n; ++i) out.buf[i] = static_cast<wchar_t>(tmp[i]);
        out.len = n;
        out.buf[n] = L'\0';
    }
    return out;
}

} // namespace usyseek
