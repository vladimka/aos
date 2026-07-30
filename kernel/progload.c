#include "fs.h"
#include "elf.h"
#include "terminal.h"
#include "syscall.h"

extern const struct embedded_prog {
    const char *name;
    const unsigned char *data;
    unsigned int size;
} embedded_progs[];

void load_embedded_programs(void) {
    terminal_print("Loading programs... ");
    for (int i = 0; embedded_progs[i].name; i++) {
        if (fs_exists(embedded_progs[i].name)) continue;
        int ret = fs_write(embedded_progs[i].name,
                          (const char *)embedded_progs[i].data,
                          embedded_progs[i].size);
        (void)ret;
    }
    terminal_print("done\n");
}

void *program_load(const char *path, const char *args) {
    syscall_set_args(args);
    return elf_load(path);
}
