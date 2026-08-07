#ifndef SYMTAB_H
#define SYMTAB_H

struct symtab_entry {
    unsigned int addr;
    const char *name;
};

extern const struct symtab_entry kernel_symtab[];
extern const unsigned int kernel_symtab_count;

#endif
