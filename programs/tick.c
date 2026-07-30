#include "libaos.h"

void _start(void) {
    print("\nTick: ");
    print_hex(get_tick());
}
