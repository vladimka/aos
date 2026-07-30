#include "libaos.h"

void _start(void) {
    print("\nAOS Kernel v0.1");
    print("\nArch: x86 (i386)");
    print("\nFeatures: GDT, IDT, PIC, PIT, Keyboard, VGA, Serial, SFS, ELF");
    print("\nTick rate: ~55ms");
}
