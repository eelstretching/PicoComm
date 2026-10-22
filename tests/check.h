// A minimal test harness: CHECK records failures and keeps going, so one run shows them all.

#pragma once

#include <cstdio>
#include <cstdlib>

namespace check {

inline int &failures() {
    static int n = 0;
    return n;
}

inline int finish(const char *suite) {
    if (failures() == 0) {
        printf("%s: all checks passed\n", suite);
        return 0;
    }
    printf("%s: %d check(s) FAILED\n", suite, failures());
    return 1;
}

}  // namespace check

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            check::failures()++;                                             \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                      \
    do {                                                                                    \
        long long va_ = (long long)(a), vb_ = (long long)(b);                               \
        if (va_ != vb_) {                                                                   \
            printf("%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, \
                   #a, va_, #b, vb_);                                                       \
            check::failures()++;                                                            \
        }                                                                                   \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                    \
    do {                                                                                         \
        double va_ = (double)(a), vb_ = (double)(b);                                             \
        if (va_ - vb_ > (tol) || vb_ - va_ > (tol)) {                                            \
            printf("%s:%d: CHECK_NEAR failed: %s (%g) vs %s (%g), tolerance %g\n", __FILE__,     \
                   __LINE__, #a, va_, #b, vb_, (double)(tol));                                   \
            check::failures()++;                                                                 \
        }                                                                                        \
    } while (0)
