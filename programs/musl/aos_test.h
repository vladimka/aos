#ifndef AOS_TEST_H
#define AOS_TEST_H

// Minimal unit-test framework for AOS userland (musl, static i386).
//
// Each test program includes this header, calls TEST_SUITE(name) at the top of
// main, sprinkles assertion macros through its checks, and finishes with a
// single TEST_PASS() call. The framework tracks pass/fail counts and prints a
// one-line summary; any failure aborts with _exit(1).
//
//   #include "aos_test.h"
//   int main(void) {
//       TEST_SUITE("fstest");
//       TEST_ASSERT(mkdir("/t", 0777) == 0);
//       TEST_ASSERT_GE(open("/t/f", O_CREAT | O_WRONLY), 0);
//       TEST_PASS();
//   }
//
// Assertion macros evaluate the condition exactly once and, on failure, print
// the suite name, the unmet condition, and the file:line where it happened.

#include <stdio.h>
#include <stdlib.h>

static int  __t_pass = 0;
static int  __t_fail = 0;
static const char *__t_name = "test";

#define TEST_SUITE(name)  __t_name = (name)

#define __TEST_CHECK(cond, msg)                                            \
    do {                                                                   \
        if (cond) {                                                        \
            __t_pass++;                                                    \
        } else {                                                           \
            __t_fail++;                                                    \
            printf("\n  FAIL %s: %s (%s:%d)",                              \
                   __t_name, msg, __FILE__, __LINE__);                     \
        }                                                                  \
    } while (0)

#define TEST_ASSERT(cond) __TEST_CHECK(cond, #cond)

#define TEST_ASSERT_EQ(a, b) _TEST_VAL(a, b, ==, "==")
#define TEST_ASSERT_NE(a, b) _TEST_VAL(a, b, !=, "!=")
#define TEST_ASSERT_GE(a, b) _TEST_VAL(a, b, >=, ">=")
#define TEST_ASSERT_GT(a, b) _TEST_VAL(a, b, >,  ">")
#define TEST_ASSERT_LT(a, b) _TEST_VAL(a, b, <,  "<")

#define _TEST_VAL(a, b, op, opstr)                                         \
    do {                                                                   \
        int _a = (a);                                                      \
        int _b = (b);                                                      \
        if (_a op _b) {                                                    \
            __t_pass++;                                                    \
        } else {                                                           \
            __t_fail++;                                                    \
            printf("\n  FAIL %s: %d %s %d (%s %s %s, %s:%d)",              \
                   __t_name, _a, opstr, _b, #a, opstr, #b,                 \
                   __FILE__, __LINE__);                                    \
        }                                                                  \
    } while (0)

#define TEST_PASS()                                                         \
    do {                                                                    \
        int _total = __t_pass + __t_fail;                                   \
        if (__t_fail) {                                                     \
            printf("\nFAIL %s: %d/%d passed\n",                             \
                   __t_name, __t_pass, _total);                             \
            _exit(1);                                                       \
        } else {                                                            \
            printf("\nPASS %s: %d/%d passed\n",                             \
                   __t_name, __t_pass, _total);                             \
        }                                                                   \
    } while (0)

#endif
