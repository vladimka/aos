#include <stdio.h>
#include "aosabi.h"

// Spawn lin/hello as a real (pid>0) Linux task with sink 0 so its stdout
// routes straight to the kernel terminal. Exercises the private-window path.
int main(void) {
    int rc = aos_spawn("lin/hello", "", 0);
    if (rc < 0) {
        printf("spawn failed rc=%d\n", rc);
        return 1;
    }
    printf("linux task %d spawned\n", rc);
    return 0;
}