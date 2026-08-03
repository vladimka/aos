#include "libaos.h"

static void fail(void) {
    print("IPC TEST FAIL\n");
    panic();
}

void main(void) {
    unsigned int start = get_tick();
    int child = spawn("bin/echo", "ipc", getpid());
    if (child < 0) fail();

    int exits = 0;
    struct aos_msg m;
    while ((int)(get_tick() - start) < 1000) {
        if (recv_msg(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            exits++;
        if (exits > 1) fail();
        if (exits == 1) break;
        yield();
    }
    if (exits != 1) fail();

    start = get_tick();
    while ((int)(get_tick() - start) < 100) {
        if (recv_msg(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            fail();
        yield();
    }
    print("IPC TEST PASS\n");
}
