#include <stdio.h>
#include <sys/reboot.h>

int main(void) {
    printf("\nRebooting...\n");
    reboot(RB_AUTOBOOT);
    return 0;
}