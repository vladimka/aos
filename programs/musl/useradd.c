#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static void append_line(const char *path, const char *line) {
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        /* Create if doesn't exist */
        fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) { fprintf(stderr, "useradd: cannot open %s\n", path); return; }
    }
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
    close(fd);
}

static void mkdir_p(const char *path) {
    mkdir(path, 0755);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: useradd <username> [uid]\n");
        return 1;
    }
    if (getuid() != 0) {
        fprintf(stderr, "useradd: must be root\n");
        return 1;
    }
    const char *name = argv[1];
    int uid = 1000;
    if (argc >= 3) uid = atoi(argv[2]);
    int gid = uid;
    /* Check if user already exists */
    {
        int fd = open("/etc/passwd", 0);
        if (fd >= 0) {
            char buf[2048];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                char prefix[64];
                snprintf(prefix, sizeof prefix, "\n%s:", name);
                if (strncmp(buf, name, strlen(name)) == 0 && buf[strlen(name)] == ':') {
                    fprintf(stderr, "useradd: user %s already exists\n", name);
                    return 1;
                }
                if (strstr(buf, prefix)) {
                    fprintf(stderr, "useradd: user %s already exists\n", name);
                    return 1;
                }
            }
        }
    }
    /* Add to /etc/passwd */
    char entry[256];
    snprintf(entry, sizeof entry, "%s:x:%d:%d::/home/%s:/bin/sh", name, uid, gid, name);
    append_line("/etc/passwd", entry);
    /* Add to /etc/group */
    snprintf(entry, sizeof entry, "%s:x:%d:", name, gid);
    append_line("/etc/group", entry);
    /* Create home directory */
    char home[64];
    snprintf(home, sizeof home, "/home/%s", name);
    mkdir_p(home);
    printf("useradd: user %s added (uid=%d)\n", name, uid);
    return 0;
}
