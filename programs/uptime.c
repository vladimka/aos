#include "libaos.h"

void _start(void) {
    unsigned int t = get_tick();
    unsigned int secs = t / 18;
    unsigned int frac = (t % 18) * 100 / 18;
    print("\nUptime: ");
    print_dec(secs);
    putchar('.');
    if (frac < 10) putchar('0');
    print_dec(frac);
    print(" seconds");
}
