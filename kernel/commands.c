#include "commands.h"
#include "terminal.h"
#include "serial.h"
#include "string.h"
#include "elf.h"
#include "syscall.h"
#include "fs.h"
#include "progload.h"

static void cmd_format(void) {
    terminal_print("\nFormatting filesystem...");
    fs_format();

    extern void load_embedded_programs(void);
    load_embedded_programs();

    terminal_print(" done");
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

    char path[32];
    unsigned int i;
    strncpy(path, "bin/", 4);
    for (i = 4; i < 31 && cmd[i - 4]; i++)
        path[i] = cmd[i - 4];
    path[i] = '\0';

    if (!fs_exists(path)) {
        strncpy(path, cmd, 28);
        path[28] = '\0';
    }

    if (!fs_exists(path)) {
        terminal_print("\nUnknown command: ");
        terminal_write(line, cmd_len);
        terminal_print(". Type 'help'");
        terminal_set_prompt();
        return;
    }

    void (*entry)(void) = program_load(path, arg);
    if (!entry) {
        terminal_print("\nFailed to load: ");
        terminal_print(cmd);
        terminal_set_prompt();
        return;
    }

    entry();
    terminal_set_prompt();
}
