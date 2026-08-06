#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int i, j, fd = -1;
    if (argc < 2) { printf("\n"); return 0; }

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '>' && argv[i][1] == '\0' && i + 1 < argc) {
            fd = open(argv[i + 1], O_CREAT | O_WRONLY | O_TRUNC);
            break;
        }
    }

    if (fd >= 0) {
        for (j = 1; j < i; j++) {
            if (j > 1) write(fd, " ", 1);
            write(fd, argv[j], strlen(argv[j]));
        }
        close(fd);
        return 0;
    }

    printf("\n");
    for (i = 1; i < argc; i++) {
        if (i > 1) printf(" ");
        printf("%s", argv[i]);
    }
    printf("\n");
    return 0;
}