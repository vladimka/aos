#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include "aosabi.h"

static void read_file(const char *path, char *buf, unsigned cap) {
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
}

static int lookup_uid(const char *user) {
    char buf[2048];
    read_file("/etc/passwd", buf, sizeof(buf));
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
    home[0] = '\0';
    char buf[2048];
    read_file("/etc/passwd", buf, sizeof(buf));
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

static int read_line(char *buf, int cap) {
    int pos = 0;
    for (;;) {
        unsigned char ch;
        int n = read(0, &ch, 1);
        if (n == 0) return 0;   // EOF
        if (n < 0) { sched_yield(); continue; }  // EAGAIN: spin
        if (ch == '\n' || ch == '\r') { write(1, "\n", 1); break; }
        if ((ch == '\b' || ch == 127) && pos > 0) {
            pos--;
            write(1, "\b \b", 3);
            continue;
        }
        if (ch >= 32 && pos < cap - 1) {
            buf[pos++] = ch;
            write(1, &ch, 1);
        }
    }
    buf[pos] = '\0';
    return pos;
}

int main(void) {
    char user[32];
    int attempts = 0;
    while (attempts < 3) {
        printf("login: ");
        fflush(stdout);
        int n = read_line(user, sizeof(user));
        if (n <= 0) return 1;
        if (user[0] == '\0') continue;

        int uid = lookup_uid(user);
        if (uid < 0) {
            printf("Login incorrect\n");
            attempts++;
            continue;
        }

        const char *home = get_home_for(user);

        /* Set environment variables for the shell */
        setenv("HOME", home, 1);
        setenv("USER", user, 1);
        setenv("LOGNAME", user, 1);
        setenv("SHELL", "/bin/sh", 1);
        setenv("TERM", "aos", 1);

        /* Set kernel credential overrides */
        char uid_buf[16], gid_buf[16];
        snprintf(uid_buf, sizeof uid_buf, "%d", uid);
        snprintf(gid_buf, sizeof gid_buf, "%d", uid);
        setenv("__AOS_UID", uid_buf, 1);
        setenv("__AOS_GID", gid_buf, 1);

        /* execvp replaces the current process with a shell */
        char *sh_argv[] = { "sh", NULL };
        execvp("bin/sh", sh_argv);
        fprintf(stderr, "login: exec failed\n");
        return 1;
    }
    printf("Too many login attempts\n");
    return 1;
}
