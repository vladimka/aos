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

// Locate `cmd` in the PATH (or as a raw path) and execute it in-place.
// Returns 1 if a program was found and ran to its exit.
static int exec_from_path(const char *cmd, const char *arg, int trace) {
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

            if (vfs_kernel_stat(full_path, &st2) == 0)
                if (try_exec(full_path, arg, trace)) return 1;
        }

        if (!has_sep) break;
        dir = next + 1;
    }

    if (vfs_kernel_stat(cmd, &st2) == 0)
        if (try_exec(cmd, arg, trace)) return 1;
    return 0;
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

void commands_execute(const char *line) {
    while (*line == ' ') line++;
    if (!*line) {
        terminal_set_prompt();
        return;
    }

    int op;
    if (find_operator(line, &op)) {
        exec_stage(line);
        terminal_set_prompt();
        return;
    }

    run_command_raw(line);
    terminal_set_prompt();
}
