#ifndef COMMANDS_H
#define COMMANDS_H

#define PATH_MAX 128

extern char command_path[PATH_MAX];

void commands_execute(const char *line);
void commands_set_path(const char *p);

// Build a normalized absolute path from the caller's absolute `cwd` and an
// input path (relative or absolute). Resolves '.', '..' and stray slashes.
// Returns 0 on success, -1 on overflow / too-long name.
int path_norm(const char *cwd, const char *in, char *out, unsigned int outsz);

#endif
