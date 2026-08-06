#include <stdio.h>
#include "aosabi.h"

// Test helper for stracelive.py: spawn bin/clock (a long-running GUI task)
// with sink=1 (the WM) and exit immediately. The child inherits the parent's
// trace_on, so /proc/<pid>/trace shows its live AOS_EXT syscall stream.
int main(void) {
    int child = aos_spawn("bin/clock", "", 1);
    printf("bgspawn: clock pid=%d\n", child);
    return 0;
}
