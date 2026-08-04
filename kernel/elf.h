#ifndef ELF_H
#define ELF_H

#define ELF_MAGIC 0x464C457F

#define PT_LOAD 1
#define ET_DYN 3
#define LINUX_ENTRY_MIN 0x01100000
#define LINUX_BASE 0x08048000

struct elf_header {
    unsigned int   magic;
    unsigned char  arch;
    unsigned char  endian;
    unsigned char  header_ver;
    unsigned char  abi;
    unsigned char  abi_ver;
    unsigned char  pad[7];
    unsigned short type;
    unsigned short machine;
    unsigned int   version;
    unsigned int   entry;
    unsigned int   phoff;
    unsigned int   shoff;
    unsigned int   flags;
    unsigned short ehsize;
    unsigned short phentsize;
    unsigned short phnum;
    unsigned short shentsize;
    unsigned short shnum;
    unsigned short shstrndx;
};

struct elf_prog_header {
    unsigned int type;
    unsigned int offset;
    unsigned int vaddr;
    unsigned int paddr;
    unsigned int filesz;
    unsigned int memsz;
    unsigned int flags;
    unsigned int align;
};

void *elf_load(const char *path);

struct linux_ctx;

int elf_probe(const char *path, int *abi);
void *elf_load_linux(const char *path, const char *args, struct linux_ctx *lc);

#endif
