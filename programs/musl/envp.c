#include <string.h>
#include <unistd.h>

extern char **environ;

int main(void) {
    for (char **e = environ; e && *e; e++) {
        size_t len = strlen(*e);
        write(1, *e, len);
        write(1, "\n", 1);
    }
    return 0;
}
