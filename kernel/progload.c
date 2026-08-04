#include "fs.h"
#include "elf.h"
#include "terminal.h"
#include "syscall.h"

extern const struct embedded_prog {
    const char *name;
    const unsigned char *data;
    unsigned int size;
} embedded_progs[];

extern const struct embedded_file {
    const char *name;
    const unsigned char *data;
    unsigned int size;
} embedded_data[];

static void write_if_absent(const char *name, const unsigned char *data,
                            unsigned int size) {
    if (fs_exists(name)) return;
    int ret = fs_write(name, (const char *)data, size);
    if (ret < 0)
        terminal_print("write failed: ");
}

void load_embedded_programs(void) {
    terminal_print("Loading programs... ");
    for (int i = 0; embedded_progs[i].name; i++)
        write_if_absent(embedded_progs[i].name, embedded_progs[i].data,
                        embedded_progs[i].size);
    terminal_print("done\n");
}

void load_embedded_data(void) {
    for (int i = 0; embedded_data[i].name; i++)
        write_if_absent(embedded_data[i].name, embedded_data[i].data,
                        embedded_data[i].size);
}

void *program_load(const char *path, const char *args) {
    syscall_set_args(args);
    return elf_load(path);
}
