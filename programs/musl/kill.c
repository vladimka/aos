#include <stdio.h>
#include <string.h>
#include "aosabi.h"

// kill <pid>: cooperative kill via AOS_KILL. The target exits with code 9 at
// its next syscall; pid 0 and unknown pids are rejected by the kernel.
int main(int argc, char **argv) {
    if (argc < 2 || !argv[1][0]) {
        printf("\nusage: kill <pid>");
        return 1;
    }
    const char *s = argv[1];
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            printf("\nusage: kill <pid>");
            return 1;
        }
    }
    unsigned int pid = 0;
    for (const char *p = s; *p; p++)
        pid = pid * 10 + (unsigned int)(*p - '0');
    if (pid == 0 || aos_kill(pid) != 0) {
        printf("\nkill: no such process");
        return 1;
    }
    printf("\nkill: pid %u signaled", pid);
    return 0;
}
