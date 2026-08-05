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
    int fd = sd_open("/bin/help", O_RDONLY);
    if (fd < 0) {
        print("BAD: cannot open file for bad-ptr test\n");
        return;
    }
    int r = sd_read(fd, (void *)0x100000, 16);
    if (r == -5)
        print("bad-ptr rejected\n");
    else {
        print("BAD: bad-ptr accepted (");
        print_dec(r);
        print(")\n");
    }
    sd_close(fd);
}
