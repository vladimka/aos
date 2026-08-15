#include <stdio.h>
#include <unistd.h>

#define SYS_SYNC 36

int main(void) {
    long n = syscall(SYS_SYNC, 0, 0, 0, 0, 0);
    printf("\nsync: flushed %ld blocks", n);
    return 0;
}