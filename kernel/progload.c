#include "vfs.h"
#include "elf.h"
#include "terminal.h"
#include "syscall.h"
#include "task.h"
#include "linux_syscall.h"
#include "printf.h"

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
    struct aos_stat st;
    if (vfs_kernel_stat(name, &st) == 0) return;
    int ret = vfs_kernel_write(name, (const char *)data, size, 0);
    if (ret < 0)
        printf("write failed: %s (rc=%d)\n", name, ret);
}

void load_embedded_programs(void) {
    printf("Loading programs... ");
    unsigned int n = 0;
    for (int i = 0; embedded_progs[i].name; i++) {
        write_if_absent(embedded_progs[i].name, embedded_progs[i].data,
                        embedded_progs[i].size);
        n++;
    }
    printf("done (%u programs)\n", n);
}

void load_embedded_data(void) {
    unsigned int n = 0;
    for (int i = 0; embedded_data[i].name; i++) {
        write_if_absent(embedded_data[i].name, embedded_data[i].data,
                        embedded_data[i].size);
        n++;
    }
    if (n) printf("Loaded %u data files\n", n);
}

#define LINUX_WIN_LO 0x08000000
#define LINUX_WIN_HI 0x08800000
#define STACK_MARGIN 0x10000

void *program_load(const char *path, const char *args) {
    syscall_set_args(args);
    int abi;
    if (elf_probe(path, &abi) < 0) return 0;
    if (abi == ABI_LINUX) {
        struct linux_ctx *lc = task_current_lctx();
        linux_ctx_init(lc);
        lc->win_lo = LINUX_WIN_LO;
        lc->win_hi = LINUX_WIN_HI;
        lc->stack_top = LINUX_WIN_HI;
        lc->mmap_cur = LINUX_WIN_HI - STACK_MARGIN;   // below the stack margin
        task_set_abi_current(ABI_LINUX);
        return elf_load_linux(path, args, lc);
    }
    return elf_load(path);
}
