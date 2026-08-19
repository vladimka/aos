#include <getopt.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int nl = 1, esc = 0, c;
    while ((c = getopt(argc, argv, "ne")) != -1) {
        switch (c) { case 'n': nl = 0; break; case 'e': esc = 1; break; default: return 1; }
    }
    for (int i = optind; i < argc; i++) {
        if (i > optind) printf(" ");
        if (esc && strcmp(argv[i], "\\n") == 0) printf("\n");
        else printf("%s", argv[i]);
    }
    if (nl) printf("\n");
    return 0;
}
