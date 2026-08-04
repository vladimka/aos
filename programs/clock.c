#include "libaos.h"

#define CW 260
#define CH 100

void main(void) {
    unsigned int my = getpid();
    struct aos_msg m = {MSG_CREATE, CW, CH, my, 0};
    send_msg(get_event_pid(), &m);

    unsigned int winid = 0, slab = 0;
    for (;;) {
        if (recv_msg(&m) == 0 && m.type == MSG_WININFO) {
            winid = m.a;
            slab = m.b;
            break;
        }
        yield();
    }

    unsigned int *win = (unsigned int *)(AOS_SLAB_BASE + slab * AOS_SLAB_SIZE);
    unsigned int last_sec = 0xFFFFFFFFu;
    struct aos_time t;

    for (;;) {
        if (recv_msg(&m) == 0 && m.type == MSG_CLOSE)
            exit();
        unsigned int t2 = get_tick();
        unsigned int sec = t2 / 1000;
        if (sec != last_sec) {
            last_sec = sec;
            fill_rect(win, CW * 4, 0, 0, CW, CH, 0x000000);
            if (get_rtc(&t) == 0) {
                char buf[32];
                unsigned int i = 0;
                buf[i++] = (char)('0' + t.hour / 10);
                buf[i++] = (char)('0' + t.hour % 10);
                buf[i++] = ':';
                buf[i++] = (char)('0' + t.minute / 10);
                buf[i++] = (char)('0' + t.minute % 10);
                buf[i++] = ':';
                buf[i++] = (char)('0' + t.second / 10);
                buf[i++] = (char)('0' + t.second % 10);
                buf[i] = 0;
                render_text(win, CW * 4, 16, 16, buf, 0x00FF80, 0x000000);
                unsigned int j = 0;
                char d[16];
                d[j++] = (char)('0' + t.day / 10);
                d[j++] = (char)('0' + t.day % 10);
                d[j++] = '.';
                d[j++] = (char)('0' + t.month / 10);
                d[j++] = (char)('0' + t.month % 10);
                d[j++] = '.';
                d[j++] = (char)('0' + (t.year / 1000) % 10);
                d[j++] = (char)('0' + (t.year / 100) % 10);
                d[j++] = (char)('0' + (t.year / 10) % 10);
                d[j++] = (char)('0' + t.year % 10);
                d[j] = 0;
                render_text(win, CW * 4, 16, 44, d, 0x9090D0, 0x000000);
            }
            char sub[24];
            sub[0] = 'A'; sub[1] = 'O'; sub[2] = 'S'; sub[3] = ' ';
            sub[4] = 'u'; sub[5] = 'p'; sub[6] = 't'; sub[7] = 'i'; sub[8] = 'm'; sub[9] = 'e';
            sub[10] = ' ';
            unsigned int up = get_uptime();
            unsigned int us = up % 60;
            unsigned int um = (up / 60) % 60;
            sub[11] = (char)('0' + um / 10); sub[12] = (char)('0' + um % 10);
            sub[13] = ':';
            sub[14] = (char)('0' + us / 10); sub[15] = (char)('0' + us % 10);
            sub[16] = 0;
            render_text(win, CW * 4, 16, 72, sub, 0x4050A0, 0x000000);
            struct aos_msg u = {MSG_UPDATE, winid, 0, 0, 0};
            send_msg(get_event_pid(), &u);
        }
        yield();
    }
}
