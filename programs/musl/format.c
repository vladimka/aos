#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'y') {
        printf("Formatting... ");
        DIR *d = opendir("/");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                unlink(e->d_name);
            }
            closedir(d);
        }
        printf("done\n");
        return 0;
    }
    printf("\nUse 'format -y' to format (removes all files)");
    return 0;
}