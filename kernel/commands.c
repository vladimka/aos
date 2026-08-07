#include "commands.h"
#include "terminal.h"
#include "serial.h"
#include "string.h"
#include "elf.h"
#include "syscall.h"
#include "vfs.h"
#include "progload.h"
#include "user.h"
#include "task.h"
#include "linux_syscall.h"
#include "trace.h"

char command_path[PATH_MAX] = "/bin";

static int shell_status_code = 0;

int shell_status(void) {
    return shell_status_code;
}

void shell_set_status(int code) {
    shell_status_code = code;
}

// ---- Environment variables (shell-local; per-task env is P1) ----
#define ENV_MAX 16
static struct {
    char name[24];
    char val[64];
} shell_env[ENV_MAX];
static unsigned int shell_env_count = 0;

static int env_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static const char *env_get(const char *name) {
    for (unsigned int i = 0; i < shell_env_count; i++)
        if (strcmp(shell_env[i].name, name) == 0)
            return shell_env[i].val;
    return 0;
}

static void env_set(const char *name, const char *val) {
    for (unsigned int i = 0; i < shell_env_count; i++)
        if (strcmp(shell_env[i].name, name) == 0) {
            strncpy(shell_env[i].val, val, 63);
            shell_env[i].val[63] = '\0';
            return;
        }
    if (shell_env_count < ENV_MAX) {
        strncpy(shell_env[shell_env_count].name, name, 23);
        shell_env[shell_env_count].name[23] = '\0';
        strncpy(shell_env[shell_env_count].val, val, 63);
        shell_env[shell_env_count].val[63] = '\0';
        shell_env_count++;
    }
}

static void cmd_export(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        for (unsigned int i = 0; i < shell_env_count; i++) {
            terminal_print("\n");
            terminal_print(shell_env[i].name);
            terminal_print("=");
            terminal_print(shell_env[i].val);
        }
        return;
    }
    char name[24];
    unsigned int n = 0;
    while (*arg && *arg != '=' && *arg != ' ' && n < 23) name[n++] = *arg++;
    name[n] = '\0';
    if (n == 0) return;
    if (*arg == '=') {
        arg++;
        while (*arg == ' ') arg++;
        env_set(name, arg);
    } else {
        env_set(name, "");
    }
}

// Expand $NAME and $? into `out`. Unknown vars become "". A '$' not followed
// by a name char or '?' is copied literally. Truncates at outsz-1.
static void shell_expand(const char *in, char *out, unsigned int outsz) {
    unsigned int o = 0;
    while (*in && o + 1 < outsz) {
        if (*in == '$' && in[1] == '?') {
            int st = shell_status();
            char tmp[12];
            unsigned int t = 0;
            if (st == 0) { tmp[t++] = '0'; }
            else {
                unsigned int v = (st < 0) ? (unsigned int)(-st) : (unsigned int)st;
                while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
            }
            while (t > 0) out[o++] = tmp[--t];
            in += 2;
            continue;
        }
        if (*in == '$' && env_name_char(in[1])) {
            char name[24];
            unsigned int n = 0;
            const char *p = in + 1;
            while (env_name_char(*p) && n < 23) name[n++] = *p++;
            name[n] = '\0';
            const char *val = env_get(name);
            if (val)
                for (unsigned int i = 0; val[i] && o + 1 < outsz; i++)
                    out[o++] = val[i];
            in = p;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
}

static void cmd_format(void) {
    terminal_print("\nFormatting filesystem...");
    vfs_format();
    terminal_print(" done");
}

void commands_set_path(const char *p) {
    strncpy(command_path, p, PATH_MAX - 1);
    command_path[PATH_MAX - 1] = '\0';
}

// Resolve a (relative or absolute) input path against the caller's absolute
// `cwd`, producing a normalized absolute path. Handles ".", "..", and runs /
// trailing slashes. cwd must already be normalized ("/" for root).
int path_norm(const char *cwd, const char *in, char *out, unsigned int outsz) {
    char tmp[PATH_MAX + 1];
    unsigned int t = 0;
    if (in && *in == '/') {
        tmp[t++] = '/';
    } else {
        unsigned int i = 0;
        while (cwd && cwd[i] && t < sizeof(tmp) - 1) tmp[t++] = cwd[i++];
        if (t == 0) tmp[t++] = '/';
    }
    const char *p = in ? in : "";
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[VFS_NAME_MAX + 2];
        unsigned int clen = 0;
        while (*p && *p != '/') {
            if (clen < VFS_NAME_MAX) comp[clen] = *p;
            clen++;
            p++;
        }
        if (clen == 1 && comp[0] == '.') continue;
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            while (t > 0 && t - 1 > 0 && tmp[t - 1] != '/') t--;
            if (t > 1) t--;              // drop the trailing slash (keep root '/')
            continue;
        }
        if (clen > VFS_NAME_MAX) return -1;
        if (t > 0 && tmp[t - 1] != '/') {
            if (t >= sizeof(tmp) - 1) return -1;
            tmp[t++] = '/';
        }
        if (t + clen >= sizeof(tmp)) return -1;
        for (unsigned int k = 0; k < clen; k++) tmp[t++] = comp[k];
    }
    while (t > 1 && tmp[t - 1] == '/') t--;   // strip trailing slashes
    if (t == 0) tmp[t++] = '/';
    tmp[t] = '\0';
    if (t >= outsz) return -1;
    for (unsigned int i = 0; i <= t; i++) out[i] = tmp[i];
    return 0;
}

static void cmd_cd(const char *path) {
    if (!*path) {
        terminal_print("\nusage: cd <path>");
        return;
    }
    struct task *t = get_current_task();
    char nb[PATH_MAX];
    if (path_norm(t->cwd, path, nb, sizeof(nb)) < 0) {
        terminal_print("\ncd: bad path");
        return;
    }
    struct aos_stat st;
    if (vfs_kernel_stat(nb, &st) != 0 || st.type != 2) {
        terminal_print("\ncd: no such directory: ");
        terminal_print(path);
        return;
    }
    strncpy(t->cwd, nb, PATH_MAX);
    t->cwd[PATH_MAX - 1] = '\0';
}

static void cmd_pwd(void) {
    terminal_print("\n");
    terminal_print(get_current_task()->cwd);
}

static int try_exec(const char *full_path, const char *arg, int trace) {
    struct task *me = get_current_task();
    if (trace) me->trace_on = 1;
    void (*entry)(void) = program_load(full_path, arg);
    if (entry) {
        if (task_current_abi() == ABI_LINUX)
            user_program_start_linux(entry, task_current_lctx()->stack_sp);
        else
            user_program_start(entry);
        if (trace) {
            trace_session_dump();
            me->trace_on = 0;
        }
        return 1;
    }
    if (trace) me->trace_on = 0;
    terminal_print("\nFailed to load: ");
    terminal_print(full_path);
    return 0;
}

// Locate `cmd` in PATH (or as a raw path). On success fills `out` (up to
// outsz bytes) with the resolved full path and returns 1; else 0.
static int path_resolve(const char *cmd, char *out, unsigned int outsz) {
    struct aos_stat st2;
    char path_copy[PATH_MAX];
    strncpy(path_copy, command_path, PATH_MAX - 1);
    path_copy[PATH_MAX - 1] = '\0';

    char *dir = path_copy;
    while (*dir) {
        char *next = dir;
        while (*next && *next != ':') next++;
        int dir_len = next - dir;
        int has_sep = (*next == ':');
        *next = '\0';

        if (dir_len > 0) {
            char full_path[32];
            int i;
            for (i = 0; i < dir_len && i < 30; i++)
                full_path[i] = dir[i];
            if (i < 31) {
                full_path[i++] = '/';
                for (unsigned int j = 0; cmd[j] && i < 31; j++, i++)
                    full_path[i] = cmd[j];
            }
            full_path[i] = '\0';

            if (vfs_kernel_stat(full_path, &st2) == 0) {
                if (out && outsz > 0) {
                    for (int k = 0; k <= i && k < (int)outsz - 1; k++)
                        out[k] = full_path[k];
                    out[outsz - 1] = '\0';
                }
                return 1;
            }
        }

        if (!has_sep) break;
        dir = next + 1;
    }

    if (vfs_kernel_stat(cmd, &st2) == 0) {
        if (out && outsz > 0) {
            int k = 0;
            while (cmd[k] && k < (int)outsz - 1) { out[k] = cmd[k]; k++; }
            out[k] = '\0';
        }
        return 1;
    }
    return 0;
}

// Run `cmd` in-place in the current task (task 0). Returns 1 if a program
// was found and ran to its exit.
static int exec_from_path(const char *cmd, const char *arg, int trace) {
    char full_path[PATH_MAX];
    if (!path_resolve(cmd, full_path, sizeof(full_path)))
        return 0;
    return try_exec(full_path, arg, trace);
}

static int cmd_is_builtin(const char *cmd) {
    return strcmp(cmd, "format") == 0 || strcmp(cmd, "setpath") == 0 ||
           strcmp(cmd, "cd") == 0 || strcmp(cmd, "pwd") == 0 ||
           strcmp(cmd, "strace") == 0 || strcmp(cmd, "export") == 0;
}

// strace <prog> [args]: run <prog> in-place with this task's syscalls traced,
// then dump the trace of this task and every task that inherited the flag.
static void cmd_strace(const char *line) {
    const char *p = line;
    while (*p && *p != ' ') p++;
    unsigned int plen = (unsigned int)(p - line);
    while (*p == ' ') p++;
    if (plen == 0) {
        terminal_print("\nusage: strace <prog> [args]");
        return;
    }
    char prog[16];
    unsigned int cl = plen < 15 ? plen : 15;
    for (unsigned int i = 0; i < cl; i++) prog[i] = line[i];
    prog[cl] = '\0';
    exec_from_path(prog, p, 1);
}

#define OP_NONE   0
#define OP_GT     1
#define OP_GTG    2
#define OP_LT     3
#define OP_PIPE   4

static const char *find_operator(const char *line, int *op) {
    const char *p = line;
    while (*p) {
        if (*p == '>' || *p == '<' || *p == '|') {
            if (p > line && *(p - 1) != ' ') { p++; continue; }
            const char *q = p + 1;
            if (*p == '>' && *q == '>') { *op = OP_GTG; return p; }
            if (*p == '>') { *op = OP_GT; return p; }
            if (*p == '<') { *op = OP_LT; return p; }
            if (*p == '|') { *op = OP_PIPE; return p; }
        }
        p++;
    }
    *op = OP_NONE;
    return 0;
}

static void run_command_raw(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;

    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    unsigned int cmd_len = arg - line;
    while (*arg == ' ') arg++;

    char cmd[16];
    unsigned int cl = cmd_len < 15 ? cmd_len : 15;
    strncpy(cmd, line, cl);
    cmd[cl] = '\0';

    if (strcmp(cmd, "format") == 0) {
        cmd_format();
        return;
    }

    if (strcmp(cmd, "setpath") == 0) {
        if (*arg) {
            commands_set_path(arg);
            terminal_print("\nPATH=");
            terminal_print(command_path);
        } else {
            terminal_print("\nPATH=");
            terminal_print(command_path);
        }
        return;
    }

    if (strcmp(cmd, "cd") == 0) {
        cmd_cd(arg);
        return;
    }

    if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
        return;
    }

    if (strcmp(cmd, "strace") == 0) {
        cmd_strace(arg);
        return;
    }

    if (strcmp(cmd, "export") == 0) {
        cmd_export(arg);
        return;
    }

    if (exec_from_path(cmd, arg, 0)) return;

    terminal_print("\nUnknown command: ");
    terminal_write(line, cmd_len);
    terminal_print(". Type 'help'");
}

static const char *skip_token(const char *p) {
    while (*p && *p != ' ') p++;
    return p;
}

static void exec_stage(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;

    int op;
    const char *op_pos = find_operator(line, &op);
    if (op == OP_NONE) {
        run_command_raw(line);
        return;
    }

    unsigned int left_len = op_pos - line;
    while (left_len > 0 && line[left_len - 1] == ' ') left_len--;

    const char *right = op_pos + (op == OP_GTG ? 2 : 1);
    while (*right == ' ') right++;

    char left_buf[LINE_BUF_SIZE];
    if (left_len >= LINE_BUF_SIZE) left_len = LINE_BUF_SIZE - 1;
    for (unsigned int i = 0; i < left_len; i++) left_buf[i] = line[i];
    left_buf[left_len] = '\0';

    struct task *t = get_current_task();

    if (op == OP_GT || op == OP_GTG) {
        const char *fn_end = skip_token(right);
        unsigned int fn_len = fn_end - right;
        if (fn_len == 0) {
            terminal_print("\nredirect: missing file name");
            return;
        }
        char fn[PATH_MAX];
        if (fn_len >= PATH_MAX) fn_len = PATH_MAX - 1;
        for (unsigned int i = 0; i < fn_len; i++) fn[i] = right[i];
        fn[fn_len] = '\0';

        struct vfs_inode *cwd = current_task_cwd();
        int flags = VFS_O_WRONLY | VFS_O_CREAT | (op == OP_GTG ? VFS_O_APPEND : VFS_O_TRUNC);
        int fd = vfs_open_fd(cwd, fn, flags);
        vfs_put(cwd);
        if (fd < 0) {
            terminal_print("\nredirect: cannot open ");
            terminal_print(fn);
            return;
        }
        int saved = t->stdout_fd;
        t->stdout_fd = fd;
        exec_stage(left_buf);
        t->stdout_fd = saved;
        vfs_close_fd(fd);
        return;
    }

    if (op == OP_LT) {
        const char *fn_end = skip_token(right);
        unsigned int fn_len = fn_end - right;
        if (fn_len == 0) {
            terminal_print("\nredirect: missing file name");
            return;
        }
        char fn[PATH_MAX];
        if (fn_len >= PATH_MAX) fn_len = PATH_MAX - 1;
        for (unsigned int i = 0; i < fn_len; i++) fn[i] = right[i];
        fn[fn_len] = '\0';

        struct vfs_inode *cwd = current_task_cwd();
        int fd = vfs_open_fd(cwd, fn, VFS_O_RDONLY);
        vfs_put(cwd);
        if (fd < 0) {
            terminal_print("\nredirect: cannot open ");
            terminal_print(fn);
            return;
        }
        int saved = t->stdin_fd;
        t->stdin_fd = fd;
        exec_stage(left_buf);
        t->stdin_fd = saved;
        vfs_close_fd(fd);
        return;
    }

    if (op == OP_PIPE) {
        struct vfs_inode *cwd = current_task_cwd();
        int fd = vfs_open_fd(cwd, "/pipe_tmp", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
        vfs_put(cwd);
        if (fd < 0) {
            terminal_print("\npipe: cannot create temp file");
            return;
        }
        int saved_out = t->stdout_fd;
        t->stdout_fd = fd;
        exec_stage(left_buf);
        t->stdout_fd = saved_out;
        vfs_close_fd(fd);

        cwd = current_task_cwd();
        int ifd = vfs_open_fd(cwd, "/pipe_tmp", VFS_O_RDONLY);
        vfs_put(cwd);
        if (ifd < 0) {
            terminal_print("\npipe: cannot read temp file");
            struct vfs_inode *ucwd = current_task_cwd();
            vfs_unlink(ucwd, "/pipe_tmp");
            vfs_put(ucwd);
            return;
        }
        int saved_in = t->stdin_fd;
        t->stdin_fd = ifd;
        run_command_raw(right);
        t->stdin_fd = saved_in;
        vfs_close_fd(ifd);

        cwd = current_task_cwd();
        vfs_unlink(cwd, "/pipe_tmp");
        vfs_put(cwd);
    }
}

// Spawn `line` (a simple "cmd args" line, no operators) as a background task.
// Prints "bg: pid N" on success and sets $?; returns 1 on success, 0 on
// failure. Builtins are NOT handled here (caller runs them inline).
static int bg_spawn(const char *line, unsigned int *out_pid) {
    while (*line == ' ') line++;
    if (!*line) return 0;

    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    unsigned int cmd_len = (unsigned int)(arg - line);
    while (*arg == ' ') arg++;

    char cmd[16];
    unsigned int cl = cmd_len < 15 ? cmd_len : 15;
    strncpy(cmd, line, cl);
    cmd[cl] = '\0';

    char full_path[PATH_MAX];
    if (!path_resolve(cmd, full_path, sizeof(full_path))) {
        terminal_print("\nUnknown command: ");
        terminal_write(line, cmd_len);
        terminal_print(". Type 'help'");
        shell_set_status(127);
        return 0;
    }

    unsigned int pid;
    if (task_spawn(full_path, arg, 0, &pid) != 0) {
        terminal_print("\nbg: spawn failed");
        shell_set_status(1);
        return 0;
    }
    shell_set_status(0);
    terminal_print("\nbg: pid ");
    terminal_print_dec(pid);
    if (out_pid) *out_pid = pid;
    return 1;
}

// Background command with a >/>>/< redirect: spawn the left side and wire the
// child's stdout/stdin fd to the opened file.
static void run_bg_redirect(const char *line, int op, const char *op_pos) {
    unsigned int left_len = (unsigned int)(op_pos - line);
    while (left_len > 0 && line[left_len - 1] == ' ') left_len--;

    const char *right = op_pos + (op == OP_GTG ? 2 : 1);
    while (*right == ' ') right++;

    char left_buf[LINE_BUF_SIZE];
    if (left_len >= LINE_BUF_SIZE) left_len = LINE_BUF_SIZE - 1;
    for (unsigned int i = 0; i < left_len; i++) left_buf[i] = line[i];
    left_buf[left_len] = '\0';

    const char *fn_end = skip_token(right);
    unsigned int fn_len = (unsigned int)(fn_end - right);
    if (fn_len == 0) {
        terminal_print("\nredirect: missing file name");
        return;
    }
    char fn[PATH_MAX];
    if (fn_len >= PATH_MAX) fn_len = PATH_MAX - 1;
    for (unsigned int i = 0; i < fn_len; i++) fn[i] = right[i];
    fn[fn_len] = '\0';

    struct vfs_inode *cwd = current_task_cwd();
    int flags = (op == OP_LT)
        ? VFS_O_RDONLY
        : (VFS_O_WRONLY | VFS_O_CREAT | (op == OP_GTG ? VFS_O_APPEND : VFS_O_TRUNC));
    int fd = vfs_open_fd(cwd, fn, flags);
    vfs_put(cwd);
    if (fd < 0) {
        terminal_print("\nredirect: cannot open ");
        terminal_print(fn);
        return;
    }

    unsigned int pid;
    if (!bg_spawn(left_buf, &pid)) {
        vfs_close_fd(fd);   // spawn failed / not found: close, nothing wired
        return;
    }

    struct task *c = task_slot(pid);
    c->fds[fd] = vfs_ofile_ptr(fd);
    if (op == OP_LT) c->stdin_fd = fd;
    else             c->stdout_fd = fd;
}

static void run_bg(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;

    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    char cmd[16];
    unsigned int cl = ((unsigned int)(arg - line)) < 15 ? (unsigned int)(arg - line) : 15;
    strncpy(cmd, line, cl);
    cmd[cl] = '\0';

    if (cmd_is_builtin(cmd)) {
        run_command_raw(line);   // builtins run inline; & is ignored
        return;
    }

    int op;
    const char *op_pos = find_operator(line, &op);
    if (op == OP_PIPE) {
        terminal_print("\nbg: pipes not supported");
        return;
    }
    if (op != OP_NONE) {
        run_bg_redirect(line, op, op_pos);
        return;
    }
    bg_spawn(line, 0);
}

void commands_execute(const char *line) {
    while (*line == ' ') line++;
    if (!*line) {
        terminal_set_prompt();
        return;
    }

    char expanded[LINE_BUF_SIZE];
    shell_expand(line, expanded, sizeof(expanded));

    // Trailing '&' = background. Strip it (and surrounding spaces).
    unsigned int len = 0;
    while (expanded[len]) len++;
    while (len > 0 && expanded[len - 1] == ' ') expanded[--len] = '\0';
    int bg = 0;
    if (len > 0 && expanded[len - 1] == '&') {
        bg = 1;
        expanded[--len] = '\0';
        while (len > 0 && expanded[len - 1] == ' ') expanded[--len] = '\0';
    }

    if (bg) {
        run_bg(expanded);
        terminal_set_prompt();
        return;
    }

    int op;
    if (find_operator(expanded, &op)) {
        exec_stage(expanded);
        terminal_set_prompt();
        return;
    }

    run_command_raw(expanded);
    terminal_set_prompt();
}
