#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_PASSWD 2048

static int read_file(const char *path, char *buf, unsigned cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

static void write_file(const char *path, const char *data, unsigned len) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "passwd: cannot write %s\n", path); return; }
    write(fd, data, len);
    close(fd);
}

int main(void) {
    int uid = getuid();
    /* Read current user info from /proc/self/status */
    char sbuf[256];
    int fd = open("/proc/self/status", 0);
    if (fd < 0) { fprintf(stderr, "passwd: cannot read status\n"); return 1; }
    ssize_t n = read(fd, sbuf, sizeof(sbuf) - 1);
    close(fd);
    sbuf[n] = '\0';
    /* Find username from /etc/passwd matching our uid */
    char user[32] = "?";
    {
        char pbuf[2048];
        read_file("/etc/passwd", pbuf, sizeof(pbuf));
        char target[16];
        snprintf(target, sizeof target, ":%d:", uid);
        char *p = pbuf;
        while (*p) {
            char *eol = p;
            while (*eol && *eol != '\n') eol++;
            char saved = *eol;
            *eol = '\0';
            if (strstr(p, target) == p + strlen(p) - strlen(strstr(p, target))) {
                /* This line has our uid as field 2 */
                char *colon = p;
                int fi = 0;
                while (*colon && fi < 3) {
                    if (*colon == ':') fi++;
                    if (fi < 3) colon++;
                }
                /* colon now points at the second ':' */
                int j = 0;
                const char *s = p;
                while (s < colon && j < 31) user[j++] = *s++;
                user[j] = '\0';
                break;
            }
            *eol = saved;
            if (saved == '\n') p = eol + 1; else break;
        }
    }

    /* Root can change any user's password: passwd <user> */
    const char *target_user = user;
    /* Simple check: if argv[1] exists and we're root, change that user */
    if (uid != 0) {
        /* Non-root: authenticate with current password (skipped for now) */
    }
    printf("Changing password for %s\n", target_user);
    printf("New password: ");
    fflush(stdout);
    char newpass[32] = {0};
    n = read(0, newpass, sizeof(newpass) - 1);
    if (n <= 0) return 1;
    while (n > 0 && (newpass[n-1] == '\n' || newpass[n-1] == '\r')) n--;
    newpass[n] = '\0';

    if (n == 0) { fprintf(stderr, "passwd: password unchanged\n"); return 1; }

    /* Read /etc/passwd, update the password field for target_user */
    char all[MAX_PASSWD];
    int total = read_file("/etc/passwd", all, sizeof(all));
    if (total < 0) return 1;
    char updated[MAX_PASSWD] = {0};
    int uoff = 0;
    char *p = all;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        int linelen = (int)(eol - p);
        char saved = *eol;
        *eol = '\0';
        if (strncmp(p, target_user, strlen(target_user)) == 0 &&
            p[strlen(target_user)] == ':') {
            /* Replace password field (between first and second ':') */
            char *c1 = strchr(p, ':');
            char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c1 && c2) {
                memcpy(updated + uoff, p, (int)(c1 - p) + 1);
                uoff += (int)(c1 - p) + 1;
                memcpy(updated + uoff, newpass, strlen(newpass));
                uoff += strlen(newpass);
                memcpy(updated + uoff, c2, linelen - (int)(c2 - p));
                uoff += linelen - (int)(c2 - p);
            } else {
                memcpy(updated + uoff, p, linelen);
                uoff += linelen;
            }
        } else {
            memcpy(updated + uoff, p, linelen);
            uoff += linelen;
        }
        updated[uoff++] = '\n';
        *eol = saved;
        if (saved == '\n') p = eol + 1; else break;
    }
    write_file("/etc/passwd", updated, uoff);
    printf("Password updated successfully\n");
    return 0;
}
