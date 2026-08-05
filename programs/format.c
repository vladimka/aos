#include "libaos.h"

void main(void) {
    char args[256];
    get_args(args, 256);
    if (args[0] == '-') {
        char *p = args;
        while (*p == ' ') p++;
        p++;
        if (*p == 'y') {
            print("Formatting... ");
            int fd = sd_open("/", O_RDONLY);
            if (fd >= 0) {
                for (;;) {
                    char name[28];
                    if (sd_readdir(fd, name, sizeof(name)) <= 0) break;
                    sd_unlink(name);
                }
                sd_close(fd);
            }
            print("done");
            return;
        }
    }
    print("\nUse 'format -y' to format (removes all files)");
}
