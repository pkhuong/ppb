#include <cstdio>
#include <cstdlib>
#include <ppb/ppb.hpp>

static int g_fail_count = 0;
static int g_check_count = 0;

static inline void
check(bool passes, const char *file, int line, const char *cond)
{
    g_check_count++;
    if (passes)
        return;

    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, cond);
    g_fail_count++;
    return;
}

#define CHECK(cond) check(!!(cond), __FILE__, __LINE__, #cond)

static void
test_smoke()
{
    CHECK(1 + 1 == 2);
}

int
main()
{
    test_smoke();

    std::printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
