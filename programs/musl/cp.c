#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int verbose;

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) { fprintf(stderr, "cp: %s: No such file or directory\n", src); return -1; }
    int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) { close(in); fprintf(stderr, "cp: %s: cannot create\n", dst); return -1; }
    char buf[1024];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0) write(out, buf, (size_t)n);
    close(in); close(out);
    if (verbose) printf("'%s' -> '%s'\n", src, dst);
    return 0;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) { fprintf(stderr, "cp: %s: No such file or directory\n", src); return -1; }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);
    if (mkdir(dst, 0777) != 0 && stat(dst, &st) != 0) { fprintf(stderr, "cp: %s: cannot create dir\n", dst); return -1; }
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char s[512], ds[512];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(ds, sizeof ds, "%s/%s", dst, e->d_name);
        copy_tree(s, ds);
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    int rec = 0, c;
    while ((c = getopt(argc, argv, "rvf")) != -1) {
        switch (c) { case 'r': rec = 1; break; case 'v': verbose = 1; break; case 'f': break; default: return 1; }
    }
    if (argc - optind < 2) { fprintf(stderr, "usage: cp [-r] [-v] src... dst\n"); return 1; }
    int nsrc = argc - optind - 1;
    const char *dst = argv[argc - 1];
    struct stat dstst;
    int dst_is_dir = (stat(dst, &dstst) == 0 && S_ISDIR(dstst.st_mode));
    int rc = 0;
    for (int i = 0; i < nsrc; i++) {
        const char *src = argv[optind + i];
        char target[512];
        if (dst_is_dir) {
            const char *base = strrchr(src, '/');
            base = base ? base + 1 : src;
            snprintf(target, sizeof target, "%s/%s", dst, base);
        } else {
            strncpy(target, dst, sizeof target - 1);
            target[sizeof target - 1] = 0;
        }
        struct stat st;
        if (stat(src, &st) == 0 && S_ISDIR(st.st_mode) && !rec) {
            fprintf(stderr, "cp: %s: is a directory (use -r)\n", src); rc = 1; continue;
        }
        if (copy_tree(src, target) != 0) rc = 1;
    }
    return rc;
}
