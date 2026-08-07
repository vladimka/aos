#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "aosabi.h"
#include "aos_test.h"

static void sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

int main(void) {
    TEST_SUITE("sleeptest");

    unsigned int t0 = aos_get_tick();
    sleep_ms(50);
    unsigned int dt = aos_get_tick() - t0;
    TEST_ASSERT_GE(dt, 45);

    int child = aos_spawn("bin/exitto", "", (unsigned int)getpid());
    TEST_ASSERT_GE(child, 0);

    unsigned int kids[4];
    int n = aos_get_children(kids, 4);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (kids[i] == (unsigned int)child) found = 1;
    TEST_ASSERT(found);

    int code = aos_waitpid((unsigned int)child);
    TEST_ASSERT_EQ(code, 7);

    n = aos_get_children(kids, 4);
    for (int i = 0; i < n; i++)
        TEST_ASSERT_NE((int)kids[i], child);

    TEST_ASSERT_EQ(aos_waitpid(9999), -1);

    TEST_PASS();
}
