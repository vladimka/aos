#include "elf.h"
#include "vga.h"
#include "serial.h"
#include "fs.h"
#include "string.h"
#include "paging.h"
#include "task.h"
#include "linux_syscall.h"

// Valid program memory region (user pages, see paging.c)
#define PROG_LOAD_MIN 0x01000000
#define PROG_LOAD_MAX 0x01100000

static void elf_error(const char *msg) {
    serial_print("ELF: ");
    serial_print(msg);
    serial_print("\n");
}

void *elf_load(const char *path) {
    int sz = fs_get_size(path);
    if (sz <= 0) { elf_error("not found: "); serial_print(path); return 0; }

    // ELF header + program header table
    char buf[4096];
    int got = fs_read_at(path, buf, sizeof(buf), 0);
    if (got <= 0) { elf_error("read failed"); return 0; }

    struct elf_header *ehdr = (struct elf_header *)buf;

    if (ehdr->magic != ELF_MAGIC)   { elf_error("bad magic"); return 0; }
    if (ehdr->arch != 1)            { elf_error("not 32-bit"); return 0; }
    if (ehdr->machine != 3)         { elf_error("not i386");  return 0; }
    if (ehdr->type != 2)            { elf_error("not executable"); return 0; }
    if (ehdr->phentsize != sizeof(struct elf_prog_header)) {
        elf_error("bad phentsize"); return 0;
    }

    if (ehdr->phoff + ehdr->phnum * ehdr->phentsize > (unsigned int)got) {
        elf_error("program headers out of range");
        return 0;
    }

    if (ehdr->entry < PROG_LOAD_MIN || ehdr->entry >= PROG_LOAD_MAX) {
        elf_error("entry point out of range");
        return 0;
    }

    struct elf_prog_header *phdr = (struct elf_prog_header *)(buf + ehdr->phoff);

    for (unsigned int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;

        unsigned int vaddr  = phdr[i].vaddr;
        unsigned int memsz  = phdr[i].memsz;
        unsigned int filesz = phdr[i].filesz;
        unsigned int offset = phdr[i].offset;

        if (vaddr < PROG_LOAD_MIN || vaddr + memsz > PROG_LOAD_MAX ||
            vaddr + memsz < vaddr) {
            elf_error("segment out of range");
            return 0;
        }
        if (filesz > memsz) filesz = memsz;

        char *dst = (char *)vaddr;

        if (filesz > 0) {
            if (fs_read_at(path, dst, filesz, offset) <= 0) {
                elf_error("segment read failed");
                return 0;
            }
        }
        for (unsigned int j = filesz; j < memsz; j++)
            dst[j] = 0;
    }

    return (void *)ehdr->entry;
}

int elf_probe(const char *path, int *abi) {
    int sz = fs_get_size(path);
    if (sz <= 0) return -1;
    char buf[128];
    if (fs_read_at(path, buf, sizeof(buf), 0) <= 0) return -1;
    struct elf_header *ehdr = (struct elf_header *)buf;
    if (ehdr->magic != ELF_MAGIC || ehdr->arch != 1 || ehdr->machine != 3)
        return -1;
    if (ehdr->type == ET_DYN || ehdr->entry >= LINUX_ENTRY_MIN)
        *abi = ABI_LINUX;
    else
        *abi = ABI_AOS;
    return 0;
}

// Build the initial user stack: argc/argv/envp/auxv + strings, returned via
// lc->stack_sp. Writes are performed top-down from lc->stack_top.
#define MAX_ARGC 16
#define STACK_MARGIN 0x10000   // pages mapped ahead of stack building
static void stack_build(struct linux_ctx *lc, const char *prog,
                        const char *args, struct elf_header *ehdr,
                        unsigned int phdr_vaddr) {
    const char *argv[MAX_ARGC];
    int argc = 0;
    argv[argc++] = prog;
    const char *p = args;
    while (p && *p && argc < MAX_ARGC - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }

    unsigned char *s = (unsigned char *)lc->stack_top;

    // AT_RANDOM: 16 pseudo-random bytes
    unsigned char rnd[16];
    for (int i = 0; i < 16; i++)
        rnd[i] = (unsigned char)(0x9e * (i + 1) + 0x37 * i + i * i);
    s -= 16;
    memcpy(s, rnd, 16);
    unsigned int rand_addr = (unsigned int)s;

    // AT_EXECFN string
    unsigned int el = strlen(prog);
    s -= el + 1;
    memcpy(s, prog, el + 1);
    unsigned int execfn_addr = (unsigned int)s;

    // argv strings (copied into the stack)
    unsigned int arg_addrs[MAX_ARGC];
    for (int i = argc - 1; i >= 0; i--) {
        unsigned int n = strlen(argv[i]);
        s -= n + 1;
        memcpy(s, argv[i], n + 1);
        arg_addrs[i] = (unsigned int)s;
    }

    // auxv
    struct auxv_pair { unsigned int a, v; } auxv[] = {
        { 3, phdr_vaddr },              // AT_PHDR
        { 4, ehdr->phentsize },         // AT_PHENT
        { 5, ehdr->phnum },             // AT_PHNUM
        { 6, 4096 },                    // AT_PAGESZ
        { 7, 0 },                       // AT_BASE
        { 9, ehdr->entry },             // AT_ENTRY
        { 11, 0 },                      // AT_UID
        { 12, 0 },                      // AT_EUID
        { 13, 0 },                      // AT_GID
        { 14, 0 },                      // AT_EGID
        { 25, rand_addr },              // AT_RANDOM
        { 31, execfn_addr },            // AT_EXECFN
        { 0, 0 },
    };
    int naux = (int)(sizeof(auxv) / sizeof(auxv[0]));
    s -= naux * 8;
    memcpy(s, auxv, naux * 8);

    // envp array = { NULL }
    unsigned int z = 0;
    s -= 4;
    memcpy(s, &z, 4);

    // argv pointer array + argc
    s -= (argc + 1) * 4;
    unsigned int *av = (unsigned int *)s;
    for (int i = 0; i < argc; i++)
        av[i] = arg_addrs[i];
    av[argc] = 0;
    s -= 4;
    *(unsigned int *)s = (unsigned int)argc;

    lc->stack_sp = ((unsigned int)s) & ~0xFu;
}

void *elf_load_linux(const char *path, const char *args, struct linux_ctx *lc) {
    int sz = fs_get_size(path);
    if (sz <= 0) { elf_error("linux: not found"); return 0; }

    char buf[4096];
    int got = fs_read_at(path, buf, sizeof(buf), 0);
    if (got <= 0) { elf_error("linux: read failed"); return 0; }

    struct elf_header *ehdr = (struct elf_header *)buf;
    if (ehdr->magic != ELF_MAGIC || ehdr->arch != 1 || ehdr->machine != 3 ||
        ehdr->phentsize != sizeof(struct elf_prog_header)) {
        elf_error("linux: bad header");
        return 0;
    }
    if (ehdr->phoff + ehdr->phnum * ehdr->phentsize > (unsigned int)got) {
        elf_error("linux: phdrs out of range");
        return 0;
    }
    if (ehdr->entry < LINUX_BASE || ehdr->entry >= lc->win_hi) {
        elf_error("linux: entry out of range");
        return 0;
    }

    // Map the stack region first (identity pages for task 0 are already there).
    for (unsigned int a = lc->stack_top - STACK_MARGIN; a < lc->stack_top; a += 0x1000)
        if (paging_map_user_page(a) < 0) { elf_error("linux: stack map failed"); return 0; }

    struct elf_prog_header *phdr = (struct elf_prog_header *)(buf + ehdr->phoff);
    unsigned int phdr_vaddr = 0;
    unsigned int brk_hi = 0;

    for (unsigned int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;

        unsigned int vaddr  = phdr[i].vaddr;
        unsigned int memsz  = phdr[i].memsz;
        unsigned int filesz = phdr[i].filesz;
        unsigned int offset = phdr[i].offset;

        if (vaddr < lc->win_lo || vaddr + memsz > lc->win_hi ||
            vaddr + memsz < vaddr) {
            elf_error("linux: segment out of range");
            return 0;
        }
        if (filesz > memsz) filesz = memsz;

        if (offset <= ehdr->phoff &&
            ehdr->phoff - offset < filesz)
            phdr_vaddr = vaddr + (ehdr->phoff - offset);

        for (unsigned int a = vaddr & ~0xFFFu; a < vaddr + memsz; a += 0x1000)
            if (paging_map_user_page(a) < 0) {
                elf_error("linux: segment map failed");
                return 0;
            }

        char *dst = (char *)vaddr;
        if (filesz > 0) {
            if (fs_read_at(path, dst, filesz, offset) <= 0) {
                elf_error("linux: segment read failed");
                return 0;
            }
        }
        for (unsigned int j = filesz; j < memsz; j++)
            dst[j] = 0;

        unsigned int end = (vaddr + memsz + 0xFFF) & ~0xFFFu;
        if (end > brk_hi) brk_hi = end;
    }

    if (!phdr_vaddr) {
        // phdr table lies before the first LOAD's file offset in unusual
        // builds; fall back to the base + phoff (covers the musl layout).
        phdr_vaddr = LINUX_BASE + ehdr->phoff;
    }

    lc->brk_base = brk_hi;
    lc->brk_cur  = brk_hi;

    stack_build(lc, path, args, ehdr, phdr_vaddr);

    // Map any stack pages below the pre-mapped margin that stack_build used.
    for (unsigned int a = lc->stack_sp & ~0xFFFu; a < lc->stack_top; a += 0x1000)
        if (paging_map_user_page(a) < 0) { elf_error("linux: stack tail map failed"); return 0; }

    return (void *)ehdr->entry;
}
