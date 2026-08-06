#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("\nUsage: rm <filename>");
        return 0;
    }
    if (unlink(argv[1]) == 0)
        printf("\nDeleted: %s", argv[1]);
    else
        printf("\nNot found: %s", argv[1]);
    return 0;
}