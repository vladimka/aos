#include <sched.h>
#include <unistd.h>
#include "aosabi.h"
#include "theme.h"

#define CW 260
#define CH 100

int main(void) {
    unsigned int my = (unsigned int)getpid();
    struct aos_msg m = {MSG_CREATE, CW, CH, my, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);

    unsigned int winid = 0, slab = 0;
    for (;;) {
        if (aos_recv(&m) == 0 && m.type == MSG_WININFO) {
            winid = m.a;
            slab = m.b;
            break;
        }
        sched_yield();
    }

    unsigned int col_bg = 0x101010;
    unsigned int col_time = 0x5B93D8;
    unsigned int col_date = 0x2D496C;
    unsigned int col_sub = 0x162436;
    theme_load();
    col_bg = theme_color("theme_text_bg", 0x101010);
    col_time = theme_color("theme_accent", 0x5B93D8);
    col_date = (col_time >> 1) & 0x7F7F7F;
    col_sub = (col_time >> 2) & 0x3F3F3F;

    unsigned int *win = (unsigned int *)(AOS_SLAB_BASE + slab * AOS_SLAB_SIZE);
    unsigned int last_sec = 0xFFFFFFFFu;
    struct aos_time t;

    for (;;) {
        if (aos_recv(&m) == 0 && m.type == MSG_CLOSE)
            return 0;
        unsigned int t2 = aos_get_tick();
        unsigned int sec = t2 / 1000;
        if (sec != last_sec) {
            last_sec = sec;
            aos_fill(win, CW * 4, 0, 0, CW, CH, col_bg);
            if (aos_get_rtc(&t) == 0) {
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
                aos_render_text(win, CW * 4, 16, 16, buf, col_time, col_bg);
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
                aos_render_text(win, CW * 4, 16, 44, d, col_date, col_bg);
            }
            char sub[24];
            sub[0] = 'A'; sub[1] = 'O'; sub[2] = 'S'; sub[3] = ' ';
            sub[4] = 'u'; sub[5] = 'p'; sub[6] = 't'; sub[7] = 'i'; sub[8] = 'm'; sub[9] = 'e';
            sub[10] = ' ';
            unsigned int up = aos_uptime();
            unsigned int us = up % 60;
            unsigned int um = (up / 60) % 60;
            sub[11] = (char)('0' + um / 10); sub[12] = (char)('0' + um % 10);
            sub[13] = ':';
            sub[14] = (char)('0' + us / 10); sub[15] = (char)('0' + us % 10);
            sub[16] = 0;
            aos_render_text(win, CW * 4, 16, 72, sub, col_sub, col_bg);
            struct aos_msg u = {MSG_UPDATE, winid, 0, 0, 0};
            aos_send((unsigned int)aos_get_event_pid(), &u);
        }
        sched_yield();
    }
}