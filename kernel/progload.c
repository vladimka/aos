#include "vfs.h"
#include "elf.h"
#include "terminal.h"
#include "syscall.h"
#include "task.h"
#include "linux_syscall.h"
#include "printf.h"

#define LINUX_WIN_LO 0x08000000
#define LINUX_WIN_HI 0x08800000
#define STACK_MARGIN 0x10000

void *program_load(const char *path, const char *args, const char *env) {
    syscall_set_args(args);
    int abi;
    if (elf_probe(path, &abi) < 0) return 0;
    printf("PLOAD: abi=%d path=[%s]\n", abi, path);
    if (abi == ABI_LINUX) {
        struct linux_ctx *lc = task_current_lctx();
        linux_ctx_init(lc);
        lc->win_lo = LINUX_WIN_LO;
        lc->win_hi = LINUX_WIN_HI;
        lc->stack_top = LINUX_WIN_HI;
        lc->mmap_cur = LINUX_WIN_HI - STACK_MARGIN;   // below the stack margin
        task_set_abi_current(ABI_LINUX);
        return elf_load_linux(path, args, lc, env);
    }
    return elf_load(path);
}
