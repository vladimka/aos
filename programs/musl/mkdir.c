#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    int parents = 0, c;
    while ((c = getopt(argc, argv, "p")) != -1) { if (c == 'p') parents = 1; else return 1; }
    if (optind >= argc) { fprintf(stderr, "mkdir: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        const char *p = argv[i];
        if (parents) {
            char path[256];
            for (int k = 1; p[k]; k++) {
                if (p[k] == '/') {
                    strncpy(path, p, (size_t)k);
                    path[k] = 0;
                    mkdir(path, 0777);
                }
            }
        }
        if (mkdir(p, 0777) == 0) continue;
        fprintf(stderr, "mkdir: cannot create directory '%s': No such file or directory\n", p);
        rc = 1;
    }
    return rc;
}
