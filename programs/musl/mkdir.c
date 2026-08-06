#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("\nUsage: mkdir <dirname>");
        return 0;
    }
    if (mkdir(argv[1], 0777) == 0)
        printf("\nCreated: %s", argv[1]);
    else
        printf("\nFailed: %s", argv[1]);
    return 0;
}