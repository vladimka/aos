#include "commands.h"
#include "terminal.h"
#include "serial.h"
#include "string.h"
#include "elf.h"
#include "syscall.h"
#include "fs.h"
#include "progload.h"
#include "user.h"
#include "task.h"
#include "linux_syscall.h"

char command_path[PATH_MAX] = "bin";

static void cmd_format(void) {
    terminal_print("\nFormatting filesystem...");
    fs_format();

    extern void load_embedded_programs(void);
    load_embedded_programs();
    extern void load_embedded_data(void);
    load_embedded_data();

    terminal_print(" done");
}

void commands_set_path(const char *p) {
    strncpy(command_path, p, PATH_MAX - 1);
    command_path[PATH_MAX - 1] = '\0';
}

static int try_exec(const char *full_path, const char *arg) {
    void (*entry)(void) = program_load(full_path, arg);
    if (entry) {
        if (task_current_abi() == ABI_LINUX)
            user_program_start_linux(entry, task_current_lctx()->stack_sp);
        else
            user_program_start(entry);
        terminal_set_prompt();  // runs after the program exits
        return 1;
    }
    terminal_print("\nFailed to load: ");
    terminal_print(full_path);
    return 0;
}

void commands_execute(const char *line) {
    while (*line == ' ') line++;
    if (!*line) {
        terminal_set_prompt();
        return;
    }

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
        terminal_set_prompt();
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
        terminal_set_prompt();
        return;
    }

    // Search PATH for command
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

            if (fs_exists(full_path))
                if (try_exec(full_path, arg)) return;
        }

        if (!has_sep) break;
        dir = next + 1;
    }

    // Fallback: try cmd as raw path
    if (fs_exists(cmd))
        if (try_exec(cmd, arg)) return;

    terminal_print("\nUnknown command: ");
    terminal_write(line, cmd_len);
    terminal_print(". Type 'help'");
    terminal_set_prompt();
}
