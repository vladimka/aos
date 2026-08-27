#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "aosabi.h"

static int read_file(const char *path, char *buf, unsigned cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

static int get_uid_for(const char *user) {
    char buf[2048];
    if (read_file("/etc/passwd", buf, sizeof(buf)) < 0) return -1;
    char *p = buf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';
        char *tok[4];
        int nt = 0;
        char *t = p;
        while (*t && nt < 4) { tok[nt++] = t; while (*t && *t != ':') t++; if (*t) *t++ = '\0'; }
        if (nt >= 4 && strcmp(tok[0], user) == 0) {
            int uid = 0;
            const char *d = tok[2];
            while (*d) uid = uid * 10 + (*d++ - '0');
            return uid;
        }
        *eol = saved;
        if (saved == '\n') p = eol + 1; else break;
    }
    return -1;
}

static const char *get_home_for(const char *user) {
    static char home[64];
    char buf[2048];
    home[0] = '\0';
    if (read_file("/etc/passwd", buf, sizeof(buf)) < 0) return "/";
    char *p = buf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';
        char *tok[6];
        int nt = 0;
        char *t = p;
        while (*t && nt < 6) { tok[nt++] = t; while (*t && *t != ':') t++; if (*t) *t++ = '\0'; }
        if (nt >= 6 && strcmp(tok[0], user) == 0) {
            strncpy(home, tok[5], sizeof(home) - 1);
            home[sizeof(home) - 1] = '\0';
            return home;
        }
        *eol = saved;
        if (saved == '\n') p = eol + 1; else break;
    }
    strcpy(home, "/");
    return home;
}

int main(int argc, char **argv) {
    const char *target = "root";
    if (argc >= 2 && argv[1][0] != '-') target = argv[1];
    int target_uid = get_uid_for(target);
    if (target_uid < 0) { fprintf(stderr, "su: unknown user %s\n", target); return 1; }
    int myuid = getuid();
    if (myuid != 0 && myuid != target_uid) {
        /* Could add password check here */
    }
    const char *home = get_home_for(target);

    /* Set environment variables for the shell */
    setenv("HOME", home, 1);
    setenv("USER", target, 1);
    setenv("LOGNAME", target, 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("TERM", "aos", 1);

    /* Set kernel credential overrides */
    char uid_buf[16], gid_buf[16];
    snprintf(uid_buf, sizeof uid_buf, "%d", target_uid);
    snprintf(gid_buf, sizeof gid_buf, "%d", target_uid);
    setenv("__AOS_UID", uid_buf, 1);
    setenv("__AOS_GID", gid_buf, 1);

    /* execvp replaces the current process with a shell */
    char *sh_argv[] = { "sh", NULL };
    execvp("bin/sh", sh_argv);
    fprintf(stderr, "su: exec failed\n");
    return 1;
}
