#include "libaos.h"

void main(void){
    print("Test\n");

    char *buf = malloc(64);
    if (!buf) {
        print("malloc failed\n");
        return;
    }
    buf[0] = 'H';
    buf[1] = 'i';
    buf[2] = 0;
    print("heap: ");
    print(buf);
    print("\n");
    free(buf);

    print("press a key...\n");
    int k = read_key();
    print("key: ");
    print_hex(k);
    print("\n");

    char tmp[16];
    int r = fs_read((char *)0x100000, tmp, 16);
    if (r == -5)
        print("bad-ptr rejected\n");
    else {
        print("BAD: bad-ptr accepted (");
        print_dec(r);
        print(")\n");
    }
}
