#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "aosabi.h"

// Test helper for stracelive.py, run as `strace bgspawn` in the kernel shell.
// Waits for the WM to register as the event consumer, spawns bin/clock as a
// traced child (inherits trace_on), lets it render for a few seconds, then
// reads back /proc/<pid>/trace and echoes it to stdout (sink 0 -> serial).
// On exit the strace session dumps this task's and the child's live traces,
// so the serial log proves both the procfs file layer and the session dump.
int main(void) {
    while (aos_get_event_pid() <= 0) {
        for (volatile unsigned int i = 0; i < 20000; i++) ;
    }
    int child = aos_spawn("bin/clock", "", 1);
    if (child < 0) {
        printf("bgspawn: spawn failed rc=%d\n", child);
        return 1;
    }
    printf("bgspawn: clock pid=%d\n", child);

    unsigned int t0 = aos_get_tick();
    while (aos_get_tick() - t0 < 3000) {
        for (volatile unsigned int i = 0; i < 20000; i++) ;
    }

    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/trace", child);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("bgspawn: open %s failed\n", path);
        return 1;
    }
    char buf[256];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        if (write(1, buf, (size_t)r) != r) break;
    }
    close(fd);
    return 0;
}
