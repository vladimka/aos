#include <dirent.h>
#include <stdio.h>

static void read_all(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("\nprocinfo: open failed: %s", path);
        return;
    }
    int c;
    while ((c = fgetc(f)) != EOF)
        putchar(c);
    fclose(f);
}

int main(void) {
    printf("\n[uptime]");
    read_all("/proc/uptime");
    printf("\n[version]");
    read_all("/proc/version");
    printf("\n[mounts]");
    read_all("/proc/mounts");
    printf("\n[list]");
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            printf("\n%s", e->d_name);
        }
        closedir(d);
    }
    printf("\nPROCINFO PASS");
    return 0;
}