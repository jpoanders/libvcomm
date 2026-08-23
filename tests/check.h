#ifndef LIBVCOMM_TEST_CHECK_H
#define LIBVCOMM_TEST_CHECK_H

#include <cstdio>
#include <cstring>

// Mini-framework de teste: sem dependência externa, compila estático, roda em
// 128 MB de RAM dentro da VM.  Suficiente para o P1.

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
    std::printf("\n%s: %d verificações, %d falhas\n", suite, checks(),
                failures());
    return failures() ? 1 : 0;
}

} // namespace test

#define CHECK(expr) ::test::report((expr), #expr, __FILE__, __LINE__)

#endif
