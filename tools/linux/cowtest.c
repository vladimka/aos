/* cowtest: Stage-1.5 regression verifying AOS copy-on-write fork isolation.

 * After fork() the parent and the child must share writable pages read-only
 * (COW), so each gets a private writable copy on its first write. This test
 * proves that the child's write to a global does NOT leak into the parent's
 * copy:
 *
 *   o global `g` starts at 0 (a writable .data page -> COW-shared at fork)
 *   o child  : g = 0x1111; read back -> must be 0x1111
 *   o parent : g = 0x2222; busy-wait (letting the child run and write its
 *              value); read g back -> must STILL be 0x2222. If parent and child
 *              still shared the same frame, the child's 0x1111 would clobber
 *              the parent's g and this check fails.
 *
 * Prints COWTEST OK only if all checks pass.
 */
#include <stdio.h>
#include <unistd.h>

volatile int g = 0;
static int fail;

static void chk(int ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        fail = 1;
    }
}

int main(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork returned %d\n", (int)pid);
        return 1;
    }

    if (pid == 0) {
        /* child: provoke a COW fault on the shared `g` page */
        g = 0x1111;
        chk(g == 0x1111, "child reads its own g");
        /* second write faults again; both must be private */
        g = 0x1313;
        chk(g == 0x1313, "child re-writes g privately");
        printf("CHILD g final=%d\n", (int)g);
        return 0;
    }

    /* parent: provoke a COW fault on the shared `g` page */
    g = 0x2222;

    /* Give the child ample time to run and overwrite its own copy. Under the
       round-robin scheduler it will execute repeatedly during this loop. */
    for (volatile unsigned int i = 0; i < 3000000; i++) ;

    chk(g == 0x2222, "parent's g not clobbered by child (COW isolation)");
    printf("PARENT g final=%d\n", (int)g);

    printf("%s\n", fail ? "COWTEST FAIL" : "COWTEST OK");
    return fail ? 1 : 0;
}
