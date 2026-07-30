#include "libaos.h"

void _start(void) {
    char args[256];
    get_args(args, 256);
    if (args[0] == '-') {
        char *p = args;
        while (*p == ' ') p++;
        p++;
        if (*p == 'y') {
            print("Formatting... ");
            fs_delete("bin/help");
            fs_delete("bin/uptime");
            fs_delete("bin/clear");
            fs_delete("bin/echo");
            fs_delete("bin/tick");
            fs_delete("bin/info");
            fs_delete("bin/reboot");
            fs_delete("bin/panic");
            fs_delete("bin/ls");
            fs_delete("bin/cat");
            fs_delete("bin/rm");
            fs_delete("bin/format");
            fs_delete("bin/shutdown");
            print("done");
            return;
        }
    }
    print("\nUse 'format -y' to format (removes all files)");
}
