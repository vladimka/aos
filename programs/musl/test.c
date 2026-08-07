#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "aosabi.h"
#include "aos_test.h"

int main(void) {
    TEST_SUITE("test");

    char *buf = malloc(64);
    TEST_ASSERT(buf != NULL);
    if (!buf) TEST_PASS();
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
    TEST_ASSERT_GE(fd, 0);
    if (fd >= 0) {
        int r = read(fd, (void *)0x100000, 16);
        TEST_ASSERT(r < 0);
        close(fd);
    }

    TEST_PASS();
}
