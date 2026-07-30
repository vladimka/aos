#include "elf.h"
#include "vga.h"
#include "serial.h"
#include "fs.h"
#include "string.h"

static void elf_error(const char *msg) {
    serial_print("ELF: ");
    serial_print(msg);
    serial_print("\n");
}

void *elf_load(const char *path) {
    char buf[4096];
    int sz = fs_get_size(path);
    if (sz <= 0) { elf_error("not found"); return 0; }
    if (sz > 4096) sz = 4096;

    struct elf_header *ehdr = (struct elf_header *)buf;
    if (fs_read(path, buf, sz) <= 0) {
        elf_error("read failed");
        return 0;
    }

    if (ehdr->magic != ELF_MAGIC)   { elf_error("bad magic"); return 0; }
    if (ehdr->arch != 1)            { elf_error("not 32-bit"); return 0; }
    if (ehdr->machine != 3)         { elf_error("not i386");  return 0; }
    if (ehdr->type != 2)            { elf_error("not executable"); return 0; }
    if (ehdr->phentsize != sizeof(struct elf_prog_header)) {
        elf_error("bad phentsize"); return 0;
    }

    if (ehdr->phoff + ehdr->phnum * ehdr->phentsize > (unsigned int)sz) {
        elf_error("program headers out of range");
        return 0;
    }

    struct elf_prog_header *phdr = (struct elf_prog_header *)(buf + ehdr->phoff);

    for (unsigned int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type == PT_LOAD) {
            unsigned int vaddr = phdr[i].vaddr;
            unsigned int memsz = phdr[i].memsz;
            unsigned int filesz = phdr[i].filesz;
            unsigned int offset = phdr[i].offset;

            char *dst = (char *)vaddr;

            if (offset + filesz > (unsigned int)sz) filesz = sz - offset;

            if (offset + filesz <= (unsigned int)sz && filesz > 0)
                for (unsigned int j = 0; j < filesz; j++)
                    dst[j] = buf[offset + j];

            for (unsigned int j = filesz; j < memsz; j++)
                dst[j] = 0;
        }
    }

    return (void *)ehdr->entry;
}
