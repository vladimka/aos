#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include "aosabi.h"

#define LBUF       256
#define MAX_ARGS   16
#define MAX_STAGES 8

static char line[LBUF];
static int len, cur;                     // byte length / byte cursor offset
static int last_status = 0;
static char shell_path[128] = "bin";

static char tab_word[64];
static int tab_word_off;
static int tab_idx;
static int tab_nmatches;
static char tab_matches[40][64];

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
    tab_idx = 0;
    if (len >= LBUF - 1) return;
    memmove(line + cur + 1, line + cur, (size_t)(len - cur));
    line[cur++] = (char)c;
    len++;
    line[len] = 0;   // keep line NUL-terminated (stale tail breaks hist_push/dedup)
    redraw();
}

static void backspace(void) {
    tab_idx = 0;
    if (cur <= 0) return;
    do { cur--; } while (cur > 0 && ((unsigned char)line[cur] & 0xC0) == 0x80);
    memmove(line + cur, line + cur + 1, (size_t)(len - cur - 1));
    len--;
    line[len] = 0;
    redraw();
}

static void delete_char(void) {
    if (cur >= len) return;
    int cl = utf8_lead((unsigned char)line[cur]);
    memmove(line + cur, line + cur + cl, (size_t)(len - cur - cl));
    len -= cl;
    line[len] = 0;
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

static char sh_last_cwd[256] = "/";

// Serialize the shell env into a double-NUL-terminated block for
// SYS_SPAWN_FDS_ENV. term_off skips the leading TERM=aos entry (used for the
// last pipeline stage so `sh TERM` survives the pipeline's env).
static void sh_build_env(char *buf, int cap, int term_off) {
    int o = 0;
    if (!term_off) {
        const char *t = "TERM=aos";
        for (int i = 0; t[i] && o < cap - 2; i++) buf[o++] = t[i];
        buf[o++] = 0;
    }
    for (int i = 0; i < var_count && o < cap - 2; i++) {
        if (strcmp(var_name[i], "TERM") == 0) continue;
        for (int j = 0; var_name[i][j] && o < cap - 2; j++) buf[o++] = var_name[i][j];
        if (o < cap - 2) buf[o++] = '=';
        for (int j = 0; var_val[i][j] && o < cap - 2; j++) buf[o++] = var_val[i][j];
        buf[o++] = 0;
    }
    buf[o] = 0;
}

#define HIST_MAX 16
static char hist[HIST_MAX][LBUF];
static int hist_count;
static int hist_cur = -1;

static void hist_push(void) {
    if (len == 0) return;
    if (hist_count > 0 && strcmp(hist[(hist_count - 1) % HIST_MAX], line) == 0) {
        hist_cur = -1;   // reset browse cursor even on duplicate
        return;
    }
    memcpy(hist[hist_count % HIST_MAX], line, (size_t)len + 1);
    hist_count++;
    hist_cur = -1;
}

static void hist_load(int idx) {
    memcpy(line, hist[idx % HIST_MAX], LBUF);
    len = (int)strlen(line);
    cur = len;
    redraw();
}

static void hist_prev(void) {
    if (hist_count == 0) return;
    if (hist_cur < 0) hist_cur = hist_count - 1;
    else if (hist_cur > hist_count - HIST_MAX && hist_cur > 0) hist_cur--;
    else return;
    hist_load(hist_cur);
}

static void hist_next(void) {
    if (hist_cur < 0) return;
    if (hist_cur == hist_count - 1) {
        hist_cur = -1;
        line[0] = 0; len = 0; cur = 0;
        redraw();
        return;
    }
    hist_cur++;
    hist_load(hist_cur);
}

static void tab_collect(void) {
    int ws = cur;
    while (ws > 0 && line[ws - 1] != ' ') ws--;
    tab_word_off = ws;
    int wl = cur - ws;
    if (wl >= (int)sizeof tab_word) wl = (int)sizeof tab_word - 1;
    memcpy(tab_word, line + ws, (size_t)wl);
    tab_word[wl] = 0;
    tab_nmatches = 0;
    char *p = shell_path;
    while (*p && tab_nmatches < 40) {
        char *sep = strchr(p, ':');
        int plen = sep ? (int)(sep - p) : (int)strlen(p);
        if (plen > 0) {
            char dir[96];
            int o = 0;
            for (int i = 0; i < plen && o < 95; i++) dir[o++] = p[i];
            dir[o] = 0;
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) && tab_nmatches < 40)
                    if (strncmp(e->d_name, tab_word, (size_t)wl) == 0)
                        strncpy(tab_matches[tab_nmatches++], e->d_name, 63);
                closedir(d);
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
}

static void tab_replace_word(const char *m) {
    int off = tab_word_off;
    while (off < len && line[off] != ' ')
        off += utf8_lead((unsigned char)line[off]);
    memmove(line + tab_word_off, line + off, (size_t)(len - off));
    len -= (off - tab_word_off);
    cur = tab_word_off;
    for (const char *s = m; *s && len < LBUF - 1; s++) line[cur++] = *s;
    len = cur;
    redraw();
}

static void tab_complete(void) {
    if (cur != len) return;
    if (tab_nmatches == 0 || tab_idx == 0) {
        tab_idx = 0;
        tab_collect();
    }
    if (tab_nmatches == 0) return;
    if (tab_nmatches == 1) {
        tab_idx = 1;
        tab_replace_word(tab_matches[0]);
        return;
    }
    if (tab_idx > 0) {                       // repeated Tab — cycle
        tab_replace_word(tab_matches[tab_idx % tab_nmatches]);
        tab_idx++;
        return;
    }
    const char *m0 = tab_matches[0];
    int pl = 0;
    for (;;) {
        int all = 1;
        for (int i = 1; i < tab_nmatches; i++)
            if (tab_matches[i][pl] != m0[pl]) { all = 0; break; }
        if (!all || !m0[pl]) break;
        pl++;
    }
    if (pl > 0) {
        tab_idx = 1;      // NOTE (plan fix): was 0. With 0, a repeated Tab
                          // re-enters tab_collect() and keeps completing the
                          // same common prefix forever — it never cycles the
                          // matches (kernel terminal behavior: 2nd Tab = next
                          // match). With 1 the next Tab replaces the word.
        char pre[64];
        int o = 0;
        for (int i = 0; i < pl && o < 63; i++) pre[o++] = m0[i];
        pre[o] = 0;
        tab_replace_word(pre);
    } else {
        tab_idx = 1;
        write(1, "\r\n", 2);
        for (int i = 0; i < tab_nmatches; i++) {
            write(1, tab_matches[i], strlen(tab_matches[i]));
            write(1, "  ", 2);
        }
        write(1, "\r\n", 2);
        redraw();
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
        char b[256];                       // valid for the whole if (tgt may point at it)
        const char *tgt;
        if (argc < 2) tgt = "/";
        else if (strcmp(argv[1], "-") == 0) tgt = sh_last_cwd;
        else if (argv[1][0] == '~') {
            const char *rest = argv[1] + 1;
            if (*rest == '/') rest++;
            snprintf(b, sizeof b, "/%s", rest);
            tgt = b;
        } else tgt = argv[1];
        char cur[256];
        getcwd(cur, sizeof cur);
        if (chdir(tgt) != 0) {
            write(1, "cd: no such directory: ", 23);
            write(1, tgt, strlen(tgt));
            write(1, "\r\n", 2);
            last_status = 1;
        } else {
            if (argc >= 2 && strcmp(argv[1], "-") == 0) {
                write(1, tgt, strlen(tgt));
                write(1, "\r\n", 2);
            }
            strncpy(sh_last_cwd, cur, sizeof sh_last_cwd - 1);
            last_status = 0;
        }
        return 1;
    }
    if (strcmp(c, "pwd") == 0) {
        char buf[256];
        if (argc >= 2 && strcmp(argv[1], "-P") == 0) { /* -P == default */ }
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

static int parse_redirs(char **argv, int *argc, const char **in_f,
                        const char **out_f, int *append) {
    int w = 0;
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= *argc) return -1;
            *out_f = argv[i + 1];
            *append = (argv[i][1] == '>');
            i++;
        } else if (strcmp(argv[i], "<") == 0) {
            if (i + 1 >= *argc) return -1;
            *in_f = argv[i + 1];
            i++;
        } else {
            argv[w++] = argv[i];
        }
    }
    *argc = w;
    return 0;
}

static void run_stage(int i, int nstages, int argc, char **argv,
                      int *pipes, int *pids, int bg) {
    const char *in_f = 0, *out_f = 0;
    int append = 0;
    if (nstages == 1) {
        if (parse_redirs(argv, &argc, &in_f, &out_f, &append) < 0) {
            write(1, "sh: bad redirect\r\n", 18);
            last_status = 1;
            return;
        }
    }
    if (argc == 0) { pids[i] = -1; return; }
    if (run_builtin(argc, argv)) {
        // NOTE (plan fix): builtins MUST run for a single command too. The
        // plan's `nstages > 1 &&` made exit/cd/pwd/export/setpath fall through
        // to path_resolve and report "Unknown command" for nstages==1. A
        // builtin inside a pipeline is still rejected.
        if (nstages > 1) {
            write(1, "sh: builtin not supported in pipeline\r\n", 39);
            last_status = 1;
        }
        pids[i] = -1;
        return;
    }
    struct aos_redir redirs[2 + MAX_ARGS + 1];
    int nr = 0;
    int has_in = 0, has_out = 0;
    if (i > 0) {
        redirs[nr].child_fd = 0; redirs[nr].global_fd = pipes[2 * (i - 1)];
        nr++; has_in = 1;
    }
    if (i + 1 < nstages) {
        redirs[nr].child_fd = 1; redirs[nr].global_fd = pipes[2 * i + 1];
        nr++; has_out = 1;
    }
    int kept[8];
    int nkeep = 0;
    if (in_f) {
        int fd = open(in_f, O_RDONLY, 0);
        if (fd < 0) {
            write(1, "sh: cannot open ", 16);
            write(1, in_f, strlen(in_f));
            write(1, "\r\n", 2);
            last_status = 1;
            return;
        }
        redirs[nr].child_fd = 0; redirs[nr].global_fd = fd; nr++;
        kept[nkeep++] = fd;
        has_in = 1;
    }
    if (out_f) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int fd = open(out_f, flags, 0644);
        if (fd < 0) {
            for (int k = 0; k < nkeep; k++) close(kept[k]);
            write(1, "sh: cannot open ", 16);
            write(1, out_f, strlen(out_f));
            write(1, "\r\n", 2);
            last_status = 1;
            return;
        }
        redirs[nr].child_fd = 1; redirs[nr].global_fd = fd; nr++;
        kept[nkeep++] = fd;
        has_out = 1;
    }
    if (!has_in) {
        redirs[nr].child_fd = 0; redirs[nr].global_fd = AOS_INHERIT_FD; nr++;
    }
    if (!has_out) {
        redirs[nr].child_fd = 1; redirs[nr].global_fd = AOS_INHERIT_FD; nr++;
    }
    redirs[nr].child_fd = 0xFFFFFFFF; redirs[nr].global_fd = 0; nr++;

    char path[160];
    if (!path_resolve(argv[0], path, sizeof path)) {
        for (int k = 0; k < nkeep; k++) close(kept[k]);
        write(1, "Unknown command: ", 17);
        write(1, argv[0], strlen(argv[0]));
        write(1, "\r\n", 2);
        last_status = 127;
        return;
    }
    char args[LBUF];
    int o = 0;
    for (int a = 1; a < argc; a++) {
        for (char *p = argv[a]; *p && o < (int)sizeof args - 2; p++) args[o++] = *p;
        args[o++] = ' ';
    }
    if (o) o--;
    args[o] = 0;

    char envb[512];
    sh_build_env(envb, sizeof envb, (out_f != 0) || (i + 1 < nstages));
    int pid = aos_spawn_env(path, args, 0, envb, redirs);
    for (int k = 0; k < nkeep; k++) close(kept[k]);
    if (pid < 0) {
        write(1, "cannot run command\r\n", 20);
        last_status = 1;
        pids[i] = -1;
        return;
    }
    pids[i] = pid;
    if (bg) {
        char b[32];
        int bn = snprintf(b, sizeof b, "bg: pid %d\r\n", pid);
        write(1, b, (size_t)bn);
    }
}

static int split_stages(char *s, char **out, int max) {
    int n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (n >= max) return -1;
        out[n++] = s;
        int i = 0;
        while (s[i] && !(s[i] == '|' && i > 0 && s[i - 1] == ' ')) i++;
        if (!s[i]) break;
        s[i] = 0;
        s = s + i + 1;
    }
    return n;
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
    hist_push();
    tab_idx = 0;
    if (len == 0) { redraw(); return; }
    // NOTE (plan fix): clear the line after submit (Task 3 fix). Without it
    // the next input appends to the stale line.
    line[0] = 0; len = 0; cur = 0;
    char exp[LBUF];
    expand(exp, sizeof exp, buf);

    int bg = 0;
    int n = (int)strlen(exp);
    if (n > 0 && exp[n - 1] == '&') { bg = 1; exp[--n] = 0; }

    char *stage[MAX_STAGES];
    int nstages = split_stages(exp, stage, MAX_STAGES);
    if (nstages < 0) {
        write(1, "sh: too many stages\r\n", 21);
        last_status = 1;
        redraw();
        return;
    }
    if (nstages == 0) { redraw(); return; }

    int pipes[2 * (MAX_STAGES - 1)];
    int npipes = nstages - 1;
    for (int i = 0; i < npipes; i++) {
        if (pipe(pipes + 2 * i) != 0) {
            write(1, "sh: pipe failed\r\n", 17);
            last_status = 1;
            redraw();
            return;
        }
    }
    int pids[MAX_STAGES];
    for (int i = 0; i < MAX_STAGES; i++) pids[i] = -1;

    for (int i = 0; i < nstages; i++) {
        char *argv[MAX_ARGS];
        int argc = tokenize(stage[i], argv, MAX_ARGS);
        if (argc == 0) { pids[i] = -1; continue; }
        run_stage(i, nstages, argc, argv, pipes, pids, bg);
    }
    for (int i = 0; i < npipes; i++) {
        close(pipes[2 * i]);
        close(pipes[2 * i + 1]);
    }
    if (!bg) {
        for (int i = 0; i < nstages; i++)
            if (pids[i] > 0) last_status = aos_waitpid((unsigned int)pids[i]);
    }
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
        case 'A': hist_prev(); break;
        case 'B': hist_next(); break;
        case 'C': cursor_right(); break;
        case 'D': cursor_left(); break;
        case 'H': cur = 0; redraw(); break;
        case 'F': cur = len; redraw(); break;
        case '~': if (esc_n == 3) delete_char(); break;
        }
        return;
    }
    if (b != '\t') tab_idx = 0;              // any non-Tab input resets cycle
    if (b == 0x1b) { in_esc = 1; return; }
    switch (b) {
    case '\r': execute(); break;
    case '\b':
    case '\x7f': backspace(); break;
    case '\t': tab_complete(); break;
    default:
        if (b >= 0x20) insert_byte(b);
        break;
    }
}

int main(void) {
    env_set("TERM", "aos");
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
