#include "libaos.h"

static void print2(unsigned int n) {
    putchar((char)('0' + n / 10));
    putchar((char)('0' + n % 10));
}

void main(void) {
    struct aos_time t;
    if (get_rtc(&t) != 0) {
        print("rtc unavailable\n");
        return;
    }
    print_dec((unsigned int)t.year);
    putchar('-'); print2((unsigned int)t.month);
    putchar('-'); print2((unsigned int)t.day);
    putchar(' '); print2((unsigned int)t.hour);
    putchar(':'); print2((unsigned int)t.minute);
    putchar(':'); print2((unsigned int)t.second);
    print("\n");
}
