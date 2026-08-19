#include <dirent.h>
#include <getopt.h>
#include <stdio.h>

static void read_all(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("\nprocinfo: open failed: %s", path); return; }
    int c;
    while ((c = fgetc(f)) != EOF) putchar(c);
    fclose(f);
}

static void list_proc(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) printf("\n%s", e->d_name);
    closedir(d);
}

int main(int argc, char **argv) {
    int all = 0, c;
    while ((c = getopt(argc, argv, "a")) != -1) if (c == 'a') all = 1;
    printf("\n[uptime]"); read_all("/proc/uptime");
    printf("\n[version]"); read_all("/proc/version");
    printf("\n[mounts]"); read_all("/proc/mounts");
    if (all) { printf("\n[list]"); list_proc(); }
    printf("\nPROCINFO PASS");
    return 0;
}
