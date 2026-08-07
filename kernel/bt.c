#include "bt.h"
#include "printf.h"
#include "symtab.h"

const char *addr_to_sym(unsigned int eip, unsigned int *off) {
    if (eip < (unsigned int)_start || eip >= (unsigned int)_end)
        return 0;
    unsigned int lo = 0, hi = kernel_symtab_count;
    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;
        if (kernel_symtab[mid].addr <= eip) lo = mid + 1;
        else                                hi = mid;
    }
    if (lo == 0) return 0;
    const struct symtab_entry *e = &kernel_symtab[lo - 1];
    *off = eip - e->addr;
    return e->name;
}

void backtrace(uint32_t *ebp, int max_frames) {
    printf("--- backtrace ---\n");
    int i = 0;
    while (ebp && i < max_frames) {
        if ((uint32_t)ebp < 0x100000)
            break;
        if ((uint32_t)ebp & 3)
            break;
        uint32_t eip = ebp[1];
        if (eip < (uint32_t)_start || eip >= (uint32_t)_end)
            break;
        unsigned int off;
        const char *nm = addr_to_sym(eip, &off);
        if (nm)
            printf("  [%d] eip=0x%x  %s+0x%x\n", i, eip, nm, off);
        else
            printf("  [%d] eip=0x%x\n", i, eip);
        uint32_t *next = (uint32_t *)ebp[0];
        if (next <= ebp)
            break;
        ebp = next;
        i++;
    }
    printf("--- end ---\n");
}
