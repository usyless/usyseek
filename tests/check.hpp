#pragma once

#include <cstdio>
#include <print>

namespace usyseek::test {
inline int failures = 0;

inline void report_failure(const char* file, int line, const char* expr) noexcept {
    try {
        std::println(stderr, "{}:{}: CHECK failed: {}", file, line, expr);
    } catch (...) {
        (void)std::fputs("CHECK failed\n", stderr);
    }
    ++failures;
}
} // namespace usyseek::test

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (const bool usyseek_check_ok_ = static_cast<bool>(expr); !usyseek_check_ok_) { \
            usyseek::test::report_failure(__FILE__, __LINE__, #expr);            \
        }                                                                        \
    } while (0)
