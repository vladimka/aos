#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int n = 10;
    int c;
    while ((c = getopt(argc, argv, "n:h")) != -1) {
        switch (c) {
        case 'n': n = atoi(optarg); break;
        case 'h': printf("usage: head [-n N] file\n"); return 0;
        default:  return 1;
        }
    }
    if (optind >= argc) { fprintf(stderr, "head: missing file operand\n"); return 1; }
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "head: %s: No such file or directory\n", argv[i]); return 1; }
        if (argc - optind > 1) printf("==> %s <==\n", argv[i]);
        int nl = 0;
        char ch;
        while (nl < n && read(fd, &ch, 1) == 1) {
            putchar(ch);
            if (ch == '\n') nl++;
        }
        close(fd);
    }
    return 0;
}
