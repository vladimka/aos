#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "aosabi.h"

int main(void) {
    printf("Test\n");

    char *buf = malloc(64);
    if (!buf) {
        printf("malloc failed\n");
        return 1;
    }
    buf[0] = 'H';
    buf[1] = 'i';
    buf[2] = 0;
    printf("heap: %s\n", buf);
    free(buf);

    printf("press a key...\n");
    int k = -1;
    while (k < 0) {
        k = aos_read_key();
        sched_yield();
    }
    printf("key: %x\n", (unsigned int)k);

    int fd = open("/bin/help", O_RDONLY);
    if (fd < 0) {
        printf("BAD: cannot open file for bad-ptr test\n");
        return 1;
    }
    int r = read(fd, (void *)0x100000, 16);
    if (r < 0)
        printf("bad-ptr rejected\n");
    else {
        printf("BAD: bad-ptr accepted (%d)\n", r);
    }
    close(fd);
    return 0;
}