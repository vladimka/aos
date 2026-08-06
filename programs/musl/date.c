#include <stdio.h>
#include "aosabi.h"

static void print2(unsigned int n) {
    putchar((char)('0' + n / 10));
    putchar((char)('0' + n % 10));
}

int main(void) {
    struct aos_time t;
    if (aos_get_rtc(&t) != 0) {
        printf("rtc unavailable\n");
        return 1;
    }
    printf("%u-", (unsigned int)t.year);
    print2((unsigned int)t.month); putchar('-');
    print2((unsigned int)t.day);   putchar(' ');
    print2((unsigned int)t.hour);  putchar(':');
    print2((unsigned int)t.minute); putchar(':');
    print2((unsigned int)t.second);
    printf("\n");
    return 0;
}