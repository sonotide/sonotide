#pragma once

#include <cstdlib>
#include <iostream>

namespace sonotide::test_support {

[[noreturn]] inline void fail_check(
    const char* expression,
    const char* file,
    const int line) {
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    std::cerr.flush();
    // `abort()` opens a modal Debug CRT dialog on Windows and can indefinitely
    // block CTest or an unattended agent run. `_Exit` still reports a failing
    // process immediately, without invoking that interactive handler.
    std::_Exit(EXIT_FAILURE);
}

}  // namespace sonotide::test_support

#define SONOTIDE_REQUIRE(condition) \
    do { \
        if (!(condition)) { \
            ::sonotide::test_support::fail_check(#condition, __FILE__, __LINE__); \
        } \
    } while (false)

// Keep test call sites concise while making their checks independent of NDEBUG.
// Unlike the standard assert macro, these checks are active in every build
// configuration and always terminate the test with a diagnostic.
#define REQUIRE(condition) SONOTIDE_REQUIRE(condition)
