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

#endif
