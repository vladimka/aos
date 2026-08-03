#include "uhci.h"
#include "pci.h"
#include "serial.h"

void usb_init(void) {
    unsigned int io_base, irq;
    if (pci_init(&io_base, &irq) != 0) {
        serial_print("USB: UHCI not found; PS/2 mouse stays active.\n");
        return;
    }
    serial_print("USB: UHCI io=0x");
    serial_print_hex(io_base);
    serial_print(" irq=0x");
    serial_print_hex(irq);
    serial_print("\n");
}
