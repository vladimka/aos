#include "libaos.h"

void _start(void) {
    print("\nTriggering kernel panic...");
    panic();
}
