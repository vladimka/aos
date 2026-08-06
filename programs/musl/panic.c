#include <stdio.h>
#include "aosabi.h"

int main(void) {
    printf("\nTriggering kernel panic...\n");
    aos_panic();
    return 0;
}