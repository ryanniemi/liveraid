#pragma once

#include <stdio.h>
#include <string.h>

static int _pass = 0, _fail = 0;

#define ASSERT(cond) do {                                               \
    if (!(cond)) {                                                      \
        fprintf(stderr, "    assert: %s  (%s:%d)\n",                   \
                #cond, __FILE__, __LINE__);                             \
        _fail++;                                                        \
        return;                                                         \
    }                                                                   \
} while (0)

#define ASSERT_INT_EQ(a, b) do {                                        \
    long long _a = (long long)(a), _b = (long long)(b);                \
    if (_a != _b) {                                                     \
        fprintf(stderr, "    assert: %s == %s  (%lld != %lld)  (%s:%d)\n", \
                #a, #b, _a, _b, __FILE__, __LINE__);                   \
        _fail++;                                                        \
        return;                                                         \
    }                                                                   \
} while (0)

#define ASSERT_STR_EQ(a, b) do {                                        \
    if (strcmp((a), (b)) != 0) {                                        \
        fprintf(stderr, "    assert: strcmp(%s, %s)  (\"%s\" != \"%s\")  (%s:%d)\n", \
                #a, #b, (a), (b), __FILE__, __LINE__);                 \
        _fail++;                                                        \
        return;                                                         \
    }                                                                   \
} while (0)

#define RUN(fn) do {                                                    \
    int _f0 = _fail;                                                    \
    (fn)();                                                             \
    if (_fail == _f0) { _pass++; printf("  pass  " #fn "\n"); }        \
    else              {          printf("  FAIL  " #fn "\n"); }         \
} while (0)

#define REPORT() do {                                                   \
    printf("\n%d passed, %d failed\n", _pass, _fail);                  \
    return _fail ? 1 : 0;                                               \
} while (0)
