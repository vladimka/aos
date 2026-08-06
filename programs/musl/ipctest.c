#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include "aosabi.h"

static void fail(void) {
    printf("IPC TEST FAIL\n");
    aos_panic();
}

int main(void) {
    unsigned int start = aos_get_tick();
    int child = aos_spawn("bin/echo", "ipc", (unsigned int)getpid());
    if (child < 0) fail();

    int exits = 0;
    struct aos_msg m;
    while ((int)(aos_get_tick() - start) < 1000) {
        if (aos_recv(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            exits++;
        if (exits > 1) fail();
        if (exits == 1) break;
        sched_yield();
    }
    if (exits != 1) fail();

    start = aos_get_tick();
    while ((int)(aos_get_tick() - start) < 100) {
        if (aos_recv(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            fail();
        sched_yield();
    }
    printf("IPC TEST PASS\n");
    return 0;
}