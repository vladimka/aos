#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("\nUsage: wc <file>");
        return 0;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { printf("\nwc: %s: No such file", argv[1]); return 1; }
    int lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    close(fd);
    printf("\n%d %d %d %s", lines, words, bytes, argv[1]);
    return 0;
}