#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXROWS 32

struct row {
    int pid;
    int ppid;
    char state[8];
    char abi[8];
    char user[16];
    char cmd[300];
};

static int is_num(const char *s) {
    if (!*s) return 0;
    while (*s)
        if (*s < '0' || *s++ > '9') return 0;
    return 1;
}

static void read_file(const char *path, char *buf, unsigned cap) {
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
}

/* Find "Key:<ws>value" at the start of a line; copy value into out. */
static void status_field(const char *st, const char *key, char *out,
                         unsigned cap) {
    unsigned klen = strlen(key);
    out[0] = '\0';
    for (const char *p = st; *p; p++) {
        if ((p == st || p[-1] == '\n') && strncmp(p, key, klen) == 0) {
            const char *v = p + klen;
            while (*v == '\t' || *v == ' ') v++;
            unsigned i = 0;
            while (*v && *v != '\n' && i < cap - 1) out[i++] = *v++;
            out[i] = '\0';
            return;
        }
    }
}

/* ---- uid → name lookup via /etc/passwd ---- */
static void uid_to_name(int uid, char *out, unsigned cap) {
    out[0] = '\0';
    int fd = open("/etc/passwd", 0);
    if (fd < 0) { snprintf(out, cap, "%d", uid); return; }
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) { snprintf(out, cap, "%d", uid); return; }
    buf[n] = '\0';
    char target[12];
    snprintf(target, sizeof target, ":%d:", uid);
    char *p = buf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';
        if (strstr(p, target)) {
            /* found: name is the first field */
            char *colon = strchr(p, ':');
            if (colon) {
                unsigned i = 0;
                while (p[i] != ':' && i < cap - 1) { out[i] = p[i]; i++; }
                out[i] = '\0';
                *eol = saved;
                return;
            }
        }
        *eol = saved;
        if (saved == '\n') p = eol + 1; else break;
    }
    snprintf(out, cap, "%d", uid);
}

int main(void) {
    DIR *d = opendir("/proc");
    if (!d) {
        printf("\nps: cannot open /proc\n");
        return 1;
    }
    struct row rows[MAXROWS];
    int n = 0;
    struct dirent *e;
    while (n < MAXROWS && (e = readdir(d))) {
        if (!is_num(e->d_name)) continue;
        char path[300], cbuf[sizeof(((struct row *)0)->cmd)], sbuf[128];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        read_file(path, cbuf, sizeof(cbuf));
        if (!cbuf[0]) continue;
        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        read_file(path, sbuf, sizeof(sbuf));
        struct row *r = &rows[n++];
        r->pid = atoi(e->d_name);
        status_field(sbuf, "PPid:", path, sizeof(path));
        r->ppid = atoi(path);
        status_field(sbuf, "State:", r->state, sizeof(r->state));
        status_field(sbuf, "Abi:", r->abi, sizeof(r->abi));
        /* lookup user from Uid field */
        char uid_str[12];
        status_field(sbuf, "Uid:", uid_str, sizeof(uid_str));
        int uid = atoi(uid_str);
        uid_to_name(uid, r->user, sizeof(r->user));
        memcpy(r->cmd, cbuf, strlen(cbuf) + 1);
    }
    closedir(d);

    printf("\n%3s %5s %-8s %-6s %-6s %s\n", "PID", "PPID", "USER", "STATE", "ABI", "CMD");
    for (int i = 0; i < n; i++)
        printf("%3d %5d %-8s %-6s %-6s %s\n",
               rows[i].pid, rows[i].ppid, rows[i].user, rows[i].state,
               rows[i].abi, rows[i].cmd);
    return 0;
}
