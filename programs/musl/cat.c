#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("\nUsage: cat <filename>");
        return 0;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        printf("\nFile not found: %s", argv[1]);
        return 0;
    }
    printf("\n");
    int c;
    while ((c = fgetc(f)) != EOF)
        putchar(c);
    fclose(f);
    return 0;
}