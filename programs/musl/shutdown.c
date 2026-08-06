#include <stdio.h>
#include <sys/reboot.h>

int main(void) {
    printf("\nShutting down...\n");
    reboot(RB_POWER_OFF);
    return 0;
}