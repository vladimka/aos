#include <stdio.h>

int main(int argc, char **argv) {
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
