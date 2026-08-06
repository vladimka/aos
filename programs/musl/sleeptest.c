#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "aosabi.h"

static void fail(void) {
    printf("SLEEPTEST FAIL\n");
    aos_panic();
}

static void sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

int main(void) {
    printf("SLEEPTEST\n");

    unsigned int t0 = aos_get_tick();
    sleep_ms(50);
    unsigned int dt = aos_get_tick() - t0;
    if (dt < 45) {
        printf("sleep: only %u ticks\n", dt);
        fail();
    }

    int child = aos_spawn("bin/exitto", "", (unsigned int)getpid());
    if (child < 0) { printf("spawn failed\n"); fail(); }

    unsigned int kids[4];
    int n = aos_get_children(kids, 4);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (kids[i] == (unsigned int)child) found = 1;
    if (!found) { printf("child missing from get_children\n"); fail(); }

    int code = aos_waitpid((unsigned int)child);
    if (code != 7) {
        printf("waitpid got %d want 7\n", code);
        fail();
    }

    n = aos_get_children(kids, 4);
    for (int i = 0; i < n; i++)
        if (kids[i] == (unsigned int)child) {
            printf("child still listed after reap\n");
            fail();
        }

    if (aos_waitpid(9999) != -1) { printf("waitpid bogus accepted\n"); fail(); }

    printf("SLEEPTEST PASS\n");
    return 0;
}