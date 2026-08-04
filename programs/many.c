#include "libaos.h"

#define CHILDREN 10
#define ROUNDS   4

static void fail(void) {
    print("MANY FAIL\n");
    panic();
}

void main(void) {
    print("MANY: start\n");
    for (int r = 0; r < ROUNDS; r++) {
        int pids[CHILDREN];
        for (int i = 0; i < CHILDREN; i++) {
            int pid = spawn("bin/echo", "m", getpid());
            if (pid < 0) fail();
            pids[i] = pid;
        }
        unsigned int start = get_tick();
        int got = 0;
        while (got < CHILDREN && (int)(get_tick() - start) < 2000) {
            struct aos_msg m;
            if (recv_msg(&m) == 0 && m.type == MSG_EXIT) {
                int known = 0;
                for (int i = 0; i < CHILDREN; i++)
                    if (pids[i] == (int)m.a) known = 1;
                if (!known) fail();
                got++;
            }
            yield();
        }
        if (got != CHILDREN) fail();
    }
    print("MANY PASS\n");
}
