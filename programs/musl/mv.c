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
    if (in < 0) { fprintf(stderr, "mv: %s: No such file or directory\n", src); return -1; }
    int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) { close(in); fprintf(stderr, "mv: %s: cannot create\n", dst); return -1; }
    char buf[1024];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0) write(out, buf, (size_t)n);
    close(in); close(out);
    if (verbose) printf("'%s' -> '%s'\n", src, dst);
    return 0;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) { fprintf(stderr, "mv: %s: No such file or directory\n", src); return -1; }
    if (!S_ISDIR(st.st_mode)) {
        if (copy_file(src, dst) != 0) return -1;
        if (unlink(src) != 0) { fprintf(stderr, "mv: %s: cannot remove\n", src); return -1; }
        return 0;
    }
    if (mkdir(dst, 0777) != 0 && stat(dst, &st) != 0) { fprintf(stderr, "mv: %s: cannot create dir\n", dst); return -1; }
    /* Snapshot the entry names before recursing: the recursion removes
     * source entries (files unlinked, subdirs rmdir'd), which shifts the
     * SFS2 dirent indices and would make readdir skip entries. */
    char names[64][32];
    int n = 0;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != 0 && n < 64) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(names[n], sizeof names[0], "%s", e->d_name);
        n++;
    }
    closedir(d);
    if (n >= 64) { fprintf(stderr, "mv: %s: directory too large\n", src); return -1; }
    int rc = 0;
    for (int i = 0; i < n; i++) {
        char s[512], ds[512];
        snprintf(s, sizeof s, "%s/%s", src, names[i]);
        snprintf(ds, sizeof ds, "%s/%s", dst, names[i]);
        if (copy_tree(s, ds) != 0) rc = -1;
    }
    if (rc == 0 && rmdir(src) != 0) { fprintf(stderr, "mv: %s: cannot remove\n", src); rc = -1; }
    return rc;
}

int main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "vf")) != -1) {
        switch (c) { case 'v': verbose = 1; break; case 'f': break; default: return 1; }
    }
    if (argc - optind < 2) { fprintf(stderr, "usage: mv [-v] src... dst\n"); return 1; }
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
        if (copy_tree(src, target) != 0)
            rc = 1;
    }
    return rc;
}
