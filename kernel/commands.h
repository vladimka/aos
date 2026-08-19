#ifndef COMMANDS_H
#define COMMANDS_H

#define PATH_MAX 128

extern char command_path[PATH_MAX];

void commands_execute(const char *line);
void commands_set_path(const char *p);

// Set the boot-time default environment (TERM=aos); called by kernel_main.
void commands_env_default(void);

// Build a normalized absolute path from the caller's absolute `cwd` and an
// input path (relative or absolute). Resolves '.', '..' and stray slashes.
// Returns 0 on success, -1 on overflow / too-long name.
int path_norm(const char *cwd, const char *in, char *out, unsigned int outsz);

// Exit status of the last command ("$?"). Set by the kernel on program exit
// (pid 0 in-place path), read by the shell's $? expansion and background
// spawn code. 0 = success, 127 = command not found.
int shell_status(void);
void shell_set_status(int code);

#endif
