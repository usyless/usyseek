#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

// Buffer types shared by every function exported from the usyseek_rust staticlib.
extern "C" {
struct UsyStr {
    const char* ptr;
    size_t len;
};
struct UsyBuf {
    char* ptr;
    size_t len;
    size_t cap;
};

void usy_buf_free(UsyBuf b);
}

namespace usyseek {

inline UsyStr ustr(std::string_view s) noexcept { return UsyStr{s.data(), s.size()}; }

/// RAII owner of a Rust-allocated buffer.
struct RustBuf {
    UsyBuf b{nullptr, 0, 0};
    RustBuf() = default;
    RustBuf(const RustBuf&) = delete;
    RustBuf& operator=(const RustBuf&) = delete;
    RustBuf(RustBuf&& o) noexcept : b{std::exchange(o.b, UsyBuf{nullptr, 0, 0})} {}
    RustBuf& operator=(RustBuf&& o) noexcept {
        if (this != &o) {
            if (b.ptr) usy_buf_free(b);
            b = std::exchange(o.b, UsyBuf{nullptr, 0, 0});
        }
        return *this;
    }
    ~RustBuf() {
        if (b.ptr) usy_buf_free(b);
    }
    [[nodiscard]] std::string_view view() const noexcept { return {b.ptr ? b.ptr : "", b.len}; }
    [[nodiscard]] std::string str() const { return std::string{view()}; }
};

} // namespace usyseek
