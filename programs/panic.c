#include "libaos.h"

void main(void) {
    print("\nTriggering kernel panic...");
    panic();
}
