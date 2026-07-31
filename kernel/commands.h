#ifndef COMMANDS_H
#define COMMANDS_H

#define PATH_MAX 128

extern char command_path[PATH_MAX];

void commands_execute(const char *line);
void commands_set_path(const char *p);

#endif
