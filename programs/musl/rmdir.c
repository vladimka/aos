#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("\nUsage: rmdir <dirname>");
        return 0;
    }
    if (rmdir(argv[1]) == 0)
        printf("\nRemoved: %s", argv[1]);
    else
        printf("\nFailed: %s", argv[1]);
    return 0;
}