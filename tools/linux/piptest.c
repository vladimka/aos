#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "gen") == 0) {
        long total = (argc > 2) ? atol(argv[2]) : 20000;
        char block[512];
        memset(block, 'x', sizeof(block));
        long left = total;
        while (left > 0) {
            int chunk = (left > (long)sizeof(block)) ? (int)sizeof(block)
                                                     : (int)left;
            if (write(1, block, (unsigned int)chunk) != chunk) return 5;
            left -= chunk;
        }
        return 0;
    }

    int fd[2];
    if (pipe(fd) != 0) { printf("PIPETEST pipe failed\n"); return 1; }
    const char *msg = "hello-pipe";
    if (write(fd[1], msg, 10) != 10) { printf("PIPETEST write failed\n"); return 2; }
    close(fd[1]);
    char buf[16];
    int n = (int)read(fd[0], buf, sizeof(buf));
    if (n != 10 || memcmp(buf, msg, 10) != 0) {
        printf("PIPETEST read mismatch %d\n", n); return 3;
    }
    n = (int)read(fd[0], buf, sizeof(buf));
    if (n != 0) { printf("PIPETEST eof %d\n", n); return 4; }
    close(fd[0]);
    printf("PIPETEST OK\n");
    return 0;
}
