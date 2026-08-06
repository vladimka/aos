#include <stdio.h>
#include "aosabi.h"

int main(void) {
    printf("\nTick: %x\n", (unsigned)aos_get_tick());
    return 0;
}