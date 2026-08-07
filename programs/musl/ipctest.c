#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include "aosabi.h"
#include "aos_test.h"

int main(void) {
    TEST_SUITE("ipctest");

    unsigned int start = aos_get_tick();
    int child = aos_spawn("bin/echo", "ipc", (unsigned int)getpid());
    TEST_ASSERT_GE(child, 0);

    int exits = 0;
    struct aos_msg m;
    while ((int)(aos_get_tick() - start) < 1000) {
        if (aos_recv(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            exits++;
        TEST_ASSERT(exits <= 1);
        if (exits > 1) break;
        if (exits == 1) break;
        sched_yield();
    }
    TEST_ASSERT_EQ(exits, 1);

    start = aos_get_tick();
    while ((int)(aos_get_tick() - start) < 100) {
        if (aos_recv(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            TEST_ASSERT(0);
        sched_yield();
    }

    TEST_PASS();
}
