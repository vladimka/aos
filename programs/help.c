#include "libaos.h"

void main(void) {
    print("\nAvailable commands:");
    int fd = sd_open("/bin", O_RDONLY);
    if (fd < 0) return;
    char name[28];
    while (sd_readdir(fd, name, sizeof(name)) > 0) {
        print(" ");
        print(name);
    }
    sd_close(fd);
}
