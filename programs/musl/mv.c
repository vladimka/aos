#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("\nUsage: mv <src> <dst>");
        return 0;
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) { printf("\nmv: %s: No such file", argv[1]); return 1; }
    int out = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC);
    if (out < 0) { close(in); printf("\nmv: %s: cannot create", argv[2]); return 1; }
    char buf[4096];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);
    close(in);
    close(out);
    unlink(argv[1]);
    printf("\nMoved: %s -> %s", argv[1], argv[2]);
    return 0;
}