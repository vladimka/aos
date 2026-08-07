#ifndef BT_H
#define BT_H

#include <stdint.h>

extern char _start[];
extern char _end[];

static inline uint32_t read_ebp(void) {
    uint32_t ebp;
    __asm__ __volatile__ ("movl %%ebp, %0" : "=r" (ebp));
    return ebp;
}

void backtrace(uint32_t *ebp, int max_frames);

// Resolve a kernel text address to the nearest preceding function symbol.
// Returns the symbol name and sets *off = eip - sym_addr, or 0 if the
// address is not in the kernel text range.
const char *addr_to_sym(unsigned int eip, unsigned int *off);

#endif
