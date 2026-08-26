#include "gui.h"
#include "theme.h"

#define CW 260
#define CH 100

static unsigned int col_bg;
static unsigned int col_time;
static unsigned int col_date;
static unsigned int col_sub;

static void on_init(void) {
    col_bg = theme_color("theme_app_bg", 0xE8E8E8);
    col_time = theme_color("theme_accent", 0x5B93D8);
    col_date = (col_time >> 1) & 0x7F7F7F;
    col_sub = (col_time >> 2) & 0x3F3F3F;
}

static void on_timer(void) {
    struct aos_time t;
    gui_fill(0, 0, CW, CH, col_bg);
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
        gui_text(16, 16, buf, col_time, col_bg);
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
        gui_text(16, 44, d, col_date, col_bg);
    }
    char sub[24];
    sub[0] = 'A'; sub[1] = 'O'; sub[2] = 'S'; sub[3] = ' ';
    sub[4] = 'u'; sub[5] = 'p'; sub[6] = 't'; sub[7] = 'i';
    sub[8] = 'm'; sub[9] = 'e'; sub[10] = ' ';
    unsigned int up = aos_uptime();
    unsigned int us = up % 60;
    unsigned int um = (up / 60) % 60;
    sub[11] = (char)('0' + um / 10);
    sub[12] = (char)('0' + um % 10);
    sub[13] = ':';
    sub[14] = (char)('0' + us / 10);
    sub[15] = (char)('0' + us % 10);
    sub[16] = 0;
    gui_text(16, 72, sub, col_sub, col_bg);
    gui_update();
}

void main(void) {
    struct gui_app app = {
        .title = "Clock",
        .width = CW, .height = CH,
        .timer_ms = 1000,
        .init = on_init,
        .on_timer = on_timer
    };
    gui_init(&app);
    gui_run();
}
