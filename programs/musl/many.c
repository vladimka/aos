#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include "aosabi.h"

#define CHILDREN 10
#define ROUNDS   4

static void fail(void) {
    printf("MANY FAIL\n");
    aos_panic();
}

int main(void) {
    printf("MANY: start\n");
    for (int r = 0; r < ROUNDS; r++) {
        int pids[CHILDREN];
        for (int i = 0; i < CHILDREN; i++) {
            int pid = aos_spawn("bin/echo", "m", (unsigned int)getpid());
            if (pid < 0) fail();
            pids[i] = pid;
        }
        unsigned int start = aos_get_tick();
        int got = 0;
        while (got < CHILDREN && (int)(aos_get_tick() - start) < 2000) {
            struct aos_msg m;
            if (aos_recv(&m) == 0 && m.type == MSG_EXIT) {
                int known = 0;
                for (int i = 0; i < CHILDREN; i++)
                    if (pids[i] == (int)m.a) known = 1;
                if (!known) fail();
                got++;
            }
            sched_yield();
        }
        if (got != CHILDREN) fail();
    }
    printf("MANY PASS\n");
    return 0;
}