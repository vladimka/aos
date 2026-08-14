#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "aosabi.h"

#define LBUF       256
#define MAX_ARGS   16
#define MAX_STAGES 8

static char line[LBUF];
static int len, cur;                     // byte length / byte cursor offset
static int last_status = 0;
static char shell_path[128] = "bin";

#define MAX_VARS 16
static char var_name[MAX_VARS][32];
static char var_val[MAX_VARS][64];
static int var_count;

static const char *prompt_str = "AOS> ";

static int utf8_lead(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 1;
}

static int vis_len(const char *s, int n) {
    int cols = 0;
    for (int i = 0; i < n; ) {
        int cl = utf8_lead((unsigned char)s[i]);
        if (i + cl > n) break;
        cols++;
        i += cl;
    }
    return cols;
}

static void redraw(void) {
    write(1, "\r", 1);
    write(1, "\x1b[K", 3);
    write(1, prompt_str, 5);
    write(1, line, (size_t)len);
    int back = vis_len(line, len) - vis_len(line, cur);
    if (back > 0) {
        char b[16];
        int bn = snprintf(b, sizeof b, "\x1b[%dD", back);
        write(1, b, (size_t)bn);
    }
}

static void insert_byte(unsigned char c) {
    if (len >= LBUF - 1) return;
    memmove(line + cur + 1, line + cur, (size_t)(len - cur));
    line[cur++] = (char)c;
    len++;
    redraw();
}

static void backspace(void) {
    if (cur <= 0) return;
    do { cur--; } while (cur > 0 && ((unsigned char)line[cur] & 0xC0) == 0x80);
    memmove(line + cur, line + cur + 1, (size_t)(len - cur - 1));
    len--;
    redraw();
}

static void delete_char(void) {
    if (cur >= len) return;
    int cl = utf8_lead((unsigned char)line[cur]);
    memmove(line + cur, line + cur + cl, (size_t)(len - cur - cl));
    len -= cl;
    redraw();
}

static void cursor_left(void) {
    if (cur <= 0) return;
    do { cur--; } while (cur > 0 && ((unsigned char)line[cur] & 0xC0) == 0x80);
    redraw();
}

static void cursor_right(void) {
    if (cur >= len) return;
    int cl = utf8_lead((unsigned char)line[cur]);
    cur += cl;
    if (cur > len) cur = len;
    redraw();
}

static const char *env_get(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_name[i], name) == 0) return var_val[i];
    return 0;
}

static void env_set(const char *name, const char *val) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_name[i], name) == 0) {
            strncpy(var_val[i], val, sizeof var_val[i] - 1);
            var_val[i][sizeof var_val[i] - 1] = 0;
            return;
        }
    if (var_count < MAX_VARS) {
        strncpy(var_name[var_count], name, sizeof var_name[0] - 1);
        strncpy(var_val[var_count], val, sizeof var_val[0] - 1);
        var_name[var_count][sizeof var_name[0] - 1] = 0;
        var_val[var_count][sizeof var_val[0] - 1] = 0;
        var_count++;
    }
}

static int tokenize(char *s, char **argv, int max) {
    int argc = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (argc >= max) break;
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return argc;
}

static int path_resolve(const char *cmd, char *out, int outsz) {
    if (strchr(cmd, '/')) {
        if (strlen(cmd) < (size_t)outsz) { strcpy(out, cmd); return 1; }
        return 0;
    }
    char *p = shell_path;
    while (*p) {
        char *sep = strchr(p, ':');
        int plen = sep ? (int)(sep - p) : (int)strlen(p);
        if (plen > 0 && plen + 1 + (int)strlen(cmd) + 1 <= outsz) {
            int o = 0;
            for (int i = 0; i < plen; i++) out[o++] = p[i];
            out[o++] = '/';
            for (const char *s = cmd; *s; s++) out[o++] = *s;
            out[o] = 0;
            if (access(out, F_OK) == 0) return 1;
        }
        if (!sep) break;
        p = sep + 1;
    }
    if (strlen(cmd) < (size_t)outsz && access(cmd, F_OK) == 0) {
        strcpy(out, cmd);
        return 1;
    }
    return 0;
}

static int run_builtin(int argc, char **argv) {
    const char *c = argv[0];
    if (strcmp(c, "exit") == 0) {
        write(1, "\r\n", 2);
        exit(0);
    }
    if (strcmp(c, "cd") == 0) {
        if (argc < 2) { write(1, "usage: cd <path>\r\n", 18); last_status = 1; return 1; }
        if (chdir(argv[1]) != 0) {
            write(1, "cd: no such directory: ", 23);
            write(1, argv[1], strlen(argv[1]));
            write(1, "\r\n", 2);
            last_status = 1;
        } else {
            last_status = 0;
        }
        return 1;
    }
    if (strcmp(c, "pwd") == 0) {
        char buf[256];
        if (getcwd(buf, sizeof buf)) {
            write(1, buf, strlen(buf));
            write(1, "\r\n", 2);
            last_status = 0;
        }
        return 1;
    }
    if (strcmp(c, "export") == 0) {
        if (argc >= 2) {
            char *eq = strchr(argv[1], '=');
            if (eq) { *eq = 0; env_set(argv[1], eq + 1); }
        }
        last_status = 0;
        return 1;
    }
    if (strcmp(c, "setpath") == 0) {
        if (argc >= 2) {
            strncpy(shell_path, argv[1], sizeof shell_path - 1);
            shell_path[sizeof shell_path - 1] = 0;
            last_status = 0;
        } else {
            write(1, shell_path, strlen(shell_path));
            write(1, "\r\n", 2);
            last_status = 0;
        }
        return 1;
    }
    return 0;
}

static void run_stage(int argc, char **argv, int bg) {
    if (run_builtin(argc, argv)) return;
    char path[160];
    if (!path_resolve(argv[0], path, sizeof path)) {
        write(1, "Unknown command: ", 17);
        write(1, argv[0], strlen(argv[0]));
        write(1, "\r\n", 2);
        last_status = 127;
        return;
    }
    char args[LBUF];
    int o = 0;
    for (int i = 1; i < argc; i++) {
        for (char *p = argv[i]; *p && o < (int)sizeof args - 2; p++) args[o++] = *p;
        args[o++] = ' ';
    }
    if (o) o--;
    args[o] = 0;
    struct aos_redir redirs[3];
    redirs[0].child_fd = 0; redirs[0].global_fd = AOS_INHERIT_FD;
    redirs[1].child_fd = 1; redirs[1].global_fd = AOS_INHERIT_FD;
    redirs[2].child_fd = 0xFFFFFFFF; redirs[2].global_fd = 0;
    int pid = aos_spawn_fds(path, args, 0, redirs);
    if (pid < 0) {
        write(1, "cannot run command\r\n", 20);
        last_status = 1;
        return;
    }
    if (bg) {
        char b[32];
        int bn = snprintf(b, sizeof b, "bg: pid %d\r\n", pid);
        write(1, b, (size_t)bn);
        return;
    }
    last_status = aos_waitpid((unsigned int)pid);
}

static int has_operator(const char *s) {
    for (; *s; s++)
        if (*s == '|' || *s == '>' || *s == '<' || *s == '&') return 1;
    return 0;
}

static void expand(char *out, int cap, const char *in) {
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        if (in[i] == '$') {
            int j = i + 1;
            if (in[j] == '?') {
                char t[16];
                int tn = snprintf(t, sizeof t, "%d", last_status);
                for (int k = 0; k < tn && o < cap - 1; k++) out[o++] = t[k];
                i = j + 1;
                continue;
            }
            if (in[j] >= 'A' && in[j] <= 'Z') {
                char nm[32];
                int nn = 0;
                while (in[j] && in[j] >= 'A' && in[j] <= 'Z' && nn < 31) nm[nn++] = in[j++];
                nm[nn] = 0;
                const char *v = env_get(nm);
                if (v) while (*v && o < cap - 1) out[o++] = *v++;
                i = j;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

static void execute(void) {
    char buf[LBUF];
    memcpy(buf, line, (size_t)len);
    buf[len] = 0;
    write(1, "\r\n", 2);
    if (len == 0) { redraw(); return; }
    line[0] = 0;
    len = 0;
    cur = 0;
    char exp[LBUF];
    expand(exp, sizeof exp, buf);

    int bg = 0;
    int n = (int)strlen(exp);
    if (n > 0 && exp[n - 1] == '&') { bg = 1; exp[--n] = 0; }

    if (has_operator(exp)) {
        write(1, "sh: pipelines and redirects: not implemented yet\r\n", 50);
        last_status = 1;
        redraw();
        return;
    }
    char *argv[MAX_ARGS];
    int argc = tokenize(exp, argv, MAX_ARGS);
    if (argc > 0) run_stage(argc, argv, bg);
    redraw();
}

static int in_esc = 0;
static int esc_n = 0;

static void handle_byte(unsigned char b) {
    if (in_esc) {
        if (in_esc == 1) {
            if (b == '[') { in_esc = 2; esc_n = 0; }
            else in_esc = 0;
            return;
        }
        if (b >= '0' && b <= '9') { esc_n = esc_n * 10 + (b - '0'); return; }
        in_esc = 0;
        if (b == ';') return;
        switch (b) {
        case 'C': cursor_right(); break;
        case 'D': cursor_left(); break;
        case 'H': cur = 0; redraw(); break;
        case 'F': cur = len; redraw(); break;
        case '~': if (esc_n == 3) delete_char(); break;
        }
        return;
    }
    if (b == 0x1b) { in_esc = 1; return; }
    switch (b) {
    case '\r': execute(); break;
    case '\b':
    case '\x7f': backspace(); break;
    case '\t': break;                        // Task 5
    default:
        if (b >= 0x20) insert_byte(b);
        break;
    }
}

int main(void) {
    redraw();
    for (;;) {
        unsigned char b;
        int r = read(0, &b, 1);
        if (r == 0) break;                   // EOF: term закрылся
        if (r < 0) { sched_yield(); continue; }  // serial: очередь пуста
        handle_byte(b);
    }
    return 0;
}
