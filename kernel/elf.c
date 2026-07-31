#include "elf.h"
#include "vga.h"
#include "serial.h"
#include "fs.h"
#include "string.h"

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
