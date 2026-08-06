#include "bt.h"
#include "printf.h"

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
        printf("  [%d] eip=0x%08x\n", i, eip);
        uint32_t *next = (uint32_t *)ebp[0];
        if (next <= ebp)
            break;
        ebp = next;
        i++;
    }
    printf("--- end ---\n");
}
