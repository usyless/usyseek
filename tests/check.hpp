#pragma once

#include <cstdio>

namespace usyseek::test {
inline int failures = 0;
}

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++usyseek::test::failures;                                           \
        }                                                                        \
    } while (0)
