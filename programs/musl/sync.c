#include <stdio.h>
#include <unistd.h>

#define SYS_SYNC 36

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == '-') {}
    long n = syscall(SYS_SYNC, 0, 0, 0, 0, 0);
    printf("\nsync: flushed %ld blocks", n);
    return 0;
}
