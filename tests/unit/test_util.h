#ifndef FWU_TEST_UTIL_H
#define FWU_TEST_UTIL_H

#include <stdio.h>

static int g_test_failures;
static int g_test_checks;

#define CHECK(cond)                                                         \
    do {                                                                    \
        g_test_checks++;                                                    \
        if (!(cond)) {                                                      \
            g_test_failures++;                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                 \
        }                                                                   \
    } while (0)

#define CHECK_EQ_U32(a, b)                                                  \
    do {                                                                    \
        unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b);     \
        g_test_checks++;                                                    \
        if (_a != _b) {                                                     \
            g_test_failures++;                                              \
            fprintf(stderr, "%s:%d: %s == %s failed: 0x%lx != 0x%lx\n",     \
                    __FILE__, __LINE__, #a, #b, _a, _b);                    \
        }                                                                   \
    } while (0)

static inline int test_summary(const char *name)
{
    printf("%s: %d checks, %d failures\n", name, g_test_checks, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}

#endif
