#include "libaos.h"

void main(void) {
    unsigned char buf[32];
    int n = get_random(buf, 32);
    if (n < 0) {
        print("random: no entropy device\n");
        return;
    }
    for (int i = 0; i < n; i++)
        print_hex(buf[i]);
    print("\n");
}
