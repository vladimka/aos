#include "libaos.h"

static void fail(void) {
    print("SLEEPTEST FAIL\n");
    panic();
}

void main(void) {
    print("SLEEPTEST\n");

    // 1. sleep_ms(50) must block for ~50 ticks (tick = 1 ms).
    unsigned int t0 = get_tick();
    sleep_ms(50);
    unsigned int dt = get_tick() - t0;
    if (dt < 45) {
        print("sleep: only ");
        print_dec(dt);
        print(" ticks\n");
        fail();
    }

    // 2. Spawn a child that exits with code 7; get_children must list it and
    //    waitpid must return 7.
    int child = spawn("bin/exitto", "", getpid());
    if (child < 0) { print("spawn failed\n"); fail(); }

    unsigned int kids[4];
    int n = get_children(kids, 4);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (kids[i] == (unsigned int)child) found = 1;
    if (!found) { print("child missing from get_children\n"); fail(); }

    int code = waitpid((unsigned int)child);
    if (code != 7) {
        print("waitpid got ");
        print_dec((unsigned int)code);
        print(" want 7\n");
        fail();
    }

    // 3. After reaping, the child must be gone from get_children.
    n = get_children(kids, 4);
    for (int i = 0; i < n; i++)
        if (kids[i] == (unsigned int)child) {
            print("child still listed after reap\n");
            fail();
        }

    // 4. waitpid on a bogus pid returns -1.
    if (waitpid(9999) != -1) { print("waitpid bogus accepted\n"); fail(); }

    print("SLEEPTEST PASS\n");
}
