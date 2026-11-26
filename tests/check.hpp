#pragma once

#include <cstdio>
#include <print>

namespace capusy::test {
inline int failures = 0;

inline void report_failure(const char* file, int line, const char* expr) noexcept {
    try {
        std::println(stderr, "{}:{}: CHECK failed: {}", file, line, expr);
    } catch (...) {
        (void)std::fputs("CHECK failed\n", stderr);
    }
    ++failures;
}
} // namespace capusy::test

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        if (const bool capusy_check_ok_ = static_cast<bool>(expr); !capusy_check_ok_) {     \
            capusy::test::report_failure(__FILE__, __LINE__, #expr);                        \
        }                                                                                   \
    } while (0)
