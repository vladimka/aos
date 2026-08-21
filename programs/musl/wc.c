#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int do_l = 1, do_w = 1, do_c = 1;
    int c;
    while ((c = getopt(argc, argv, "lwc")) != -1) {
        switch (c) {
        case 'l': do_l = 1; do_w = 0; do_c = 0; break;
        case 'w': do_l = 0; do_w = 1; do_c = 0; break;
        case 'c': do_l = 0; do_w = 0; do_c = 1; break;
        default: return 1;
        }
    }
    if (optind >= argc) { fprintf(stderr, "wc: missing file operand\n"); return 1; }
    int t_l = 0, t_w = 0, t_c = 0;
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "wc: %s: No such file or directory\n", argv[i]); rc = 1; continue; }
        int lines = 0, words = 0, bytes = 0, in_word = 0;
        char buf[1024];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            bytes += n;
            for (int k = 0; k < n; k++) {
                char ch = buf[k];
                if (ch == '\n') lines++;
                if (ch == ' ' || ch == '\n' || ch == '\t') in_word = 0;
                else if (!in_word) { in_word = 1; words++; }
            }
        }
        close(fd);
        if (do_l) printf("%d ", lines);
        if (do_w) printf("%d ", words);
        if (do_c) printf("%d ", bytes);
        printf("%s\n", argv[i]);
        t_l += lines; t_w += words; t_c += bytes;
    }
    if (argc - optind > 1) {
        if (do_l) printf("%d ", t_l);
        if (do_w) printf("%d ", t_w);
        if (do_c) printf("%d ", t_c);
        printf("total\n");
    }
    return rc;
}
