#include <stdio.h>
#include "aosabi.h"

int main(void) {
    unsigned int t = aos_get_tick();
    unsigned int secs = t / 1000;
    unsigned int frac = (t % 1000) * 100 / 1000;
    printf("\nUptime: %u.%02u seconds\n", secs, frac);
    return 0;
}