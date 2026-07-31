#include "libaos.h"

void main(void) {
    unsigned int t = get_tick();
    unsigned int secs = t / 1000;
    unsigned int frac = (t % 1000) * 100 / 1000;
    print("\nUptime: ");
    print_dec(secs);
    putchar('.');
    if (frac < 10) putchar('0');
    print_dec(frac);
    print(" seconds");
}
