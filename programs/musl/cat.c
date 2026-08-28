#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int number = 0, c;
    while ((c = getopt(argc, argv, "nu")) != -1) {
        switch (c) { case 'n': number = 1; break; default: return 1; }
    }
    int rc = 0;
    if (optind >= argc) {                       // stdin
        char ch;
        while (read(0, &ch, 1) == 1) putchar(ch);
        return 0;
    }
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cat: %s: No such file or directory\n", argv[i]); rc = 1; continue; }
        if (number) {
            int lineno = 1, bol = 1;
            char ch;
            while (read(fd, &ch, 1) == 1) {
                if (bol) { printf("%6d\t", lineno++); bol = 0; }
                putchar(ch);
                if (ch == '\n') bol = 1;
            }
        } else {
            char buf[1024];
            int n, last = '\n';
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                write(1, buf, (size_t)n);
                last = buf[n - 1];
            }
            if (last != '\n') putchar('\n');
        }
        close(fd);
    }
    return rc;
}