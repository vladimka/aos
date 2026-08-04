#ifndef PROGLOAD_H
#define PROGLOAD_H

void load_embedded_programs(void);
void load_embedded_data(void);
void *program_load(const char *path, const char *args);

#endif
