#include "libaos.h"

void main(void) {
    print("\nFiles:");
    int fd = sd_open("/", O_RDONLY);
    if (fd < 0) {
        print("\n  (cannot open /)");
        return;
    }
    int found = 0;
    char name[28];
    while (sd_readdir(fd, name, sizeof(name)) > 0) {
        found = 1;
        print("\n  ");
        print(name);
        print(" (");
        unsigned int size = 0;
        struct aos_stat st;
        if (sd_stat(name, &st) == 0) size = st.size;
        print_hex(size);
        print(" bytes)");
    }
    sd_close(fd);
    if (!found) print(" (empty)");
}
