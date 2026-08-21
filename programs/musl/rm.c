#include <dirent.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int force;

static int rm_one(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) {
        if (!force) fprintf(stderr, "rm: %s: No such file or directory\n", p);
        return force ? 0 : 1;
    }
    if (!S_ISDIR(st.st_mode)) { unlink(p); return 0; }
    DIR *d = opendir(p);
    if (!d) { fprintf(stderr, "rm: %s: cannot open\n", p); return 1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char s[512];
        snprintf(s, sizeof s, "%s/%s", p, e->d_name);
        rm_one(s);
    }
    closedir(d);
    rmdir(p);
    return 0;
}

int main(int argc, char **argv) {
    int rec = 0, c;
    while ((c = getopt(argc, argv, "rf")) != -1) {
        switch (c) { case 'r': rec = 1; break; case 'f': force = 1; break; default: return 1; }
    }
    if (optind >= argc) { fprintf(stderr, "rm: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode) && !rec) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", argv[i]); rc = 1; continue;
        }
        if (rm_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
