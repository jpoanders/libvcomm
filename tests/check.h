#ifndef LIBVCOMM_TEST_CHECK_H
#define LIBVCOMM_TEST_CHECK_H

#include <cstdio>
#include <cstring>

// mini test framework: no external dependencies, compiles statically

namespace test {

inline int & failures()
{
    static int f = 0;
    return f;
}
inline int & checks()
{
    static int c = 0;
    return c;
}

inline void report(bool ok, const char * what, const char * file, int line)
{
    checks()++;
    if (ok)
        std::printf("  ok   %s\n", what);
    else {
        failures()++;
        std::printf("  FAIL %s   (%s:%d)\n", what, file, line);
    }
}

inline int summary(const char * suite)
{
    std::printf("\n%s: %d checks, %d failures\n", suite, checks(), failures());
    return failures() ? 1 : 0;
}

} // namespace test

#define CHECK(expr) ::test::report((expr), #expr, __FILE__, __LINE__)

#endif
