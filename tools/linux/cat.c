#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        int c;
        while ((c = getchar()) != EOF)
            putchar(c);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            printf("cat: %s: No such file or directory\n", argv[i]);
            return 1;
        }
        int c;
        while ((c = fgetc(f)) != EOF)
            putchar(c);
        fclose(f);
    }
    return 0;
}
