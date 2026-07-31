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

    for (;;) {
        unsigned int t = get_tick();
        unsigned int sec = t / 1000;
        if (sec != last_sec) {
            last_sec = sec;
            fill_rect(win, CW * 4, 0, 0, CW, CH, 0x000000);
            unsigned int ms = t % 1000;
            unsigned int ss = sec % 60;
            unsigned int mm = (sec / 60) % 60;
            unsigned int hh = (sec / 3600) % 24;
            char buf[16];
            buf[0] = (char)('0' + hh / 10);
            buf[1] = (char)('0' + hh % 10);
            buf[2] = ':';
            buf[3] = (char)('0' + mm / 10);
            buf[4] = (char)('0' + mm % 10);
            buf[5] = ':';
            buf[6] = (char)('0' + ss / 10);
            buf[7] = (char)('0' + ss % 10);
            buf[8] = '.';
            buf[9] = (char)('0' + ms / 100);
            buf[10] = (char)('0' + (ms / 10) % 10);
            buf[11] = (char)('0' + ms % 10);
            buf[12] = 0;
            render_text(win, CW * 4, 16, 16, buf, 0x00FF80, 0x000000);
            char sub[8];
            sub[0] = 'A';
            sub[1] = 'O';
            sub[2] = 'S';
            sub[3] = 0;
            render_text(win, CW * 4, 16, 60, sub, 0x4050A0, 0x000000);
            struct aos_msg u = {MSG_UPDATE, winid, 0, 0, 0};
            send_msg(get_event_pid(), &u);
        }
        yield();
    }
}
