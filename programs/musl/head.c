#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("\nUsage: head <file> <lines>");
        return 0;
    }
    int lines = 10;
    if (argc > 2) lines = atoi(argv[2]);
    if (lines <= 0) lines = 10;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { printf("\nhead: %s: No such file", argv[1]); return 1; }
    int nl = 0;
    char c;
    while (nl < lines && read(fd, &c, 1) == 1) {
        putchar(c);
        if (c == '\n') nl++;
    }
    close(fd);
    return 0;
}