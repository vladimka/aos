#include "libaos.h"

// Prints the three procfs entries (/proc/uptime, /proc/version, /proc/mounts)
// plus a readdir listing of /proc. Used by scripts/mounttest.py as a single
// runnable command (the WM takes over serial input, so a test can only execute
// one program in the early boot window).

static void read_all(const char *path) {
    int fd = sd_open(path, O_RDONLY);
    if (fd < 0) {
        print("\nprocinfo: open failed: ");
        print(path);
        return;
    }
    char buf[128];
    int n;
    while ((n = sd_read(fd, buf, sizeof(buf))) > 0) {
        buf[n] = '\0';
        print(buf);
    }
    sd_close(fd);
}

void main(void) {
    print("\n[uptime]");
    read_all("/proc/uptime");
    print("\n[version]");
    read_all("/proc/version");
    print("\n[mounts]");
    read_all("/proc/mounts");
    print("\n[list]");
    int fd = sd_open("/proc", O_RDONLY);
    if (fd >= 0) {
        char name[28];
        while (sd_readdir(fd, name, sizeof(name)) > 0) {
            print("\n");
            print(name);
        }
        sd_close(fd);
    }
    print("\nPROCINFO PASS");
}