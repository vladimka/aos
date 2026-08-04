#include "ports.h"
#include "rtc.h"

static unsigned char rtc_read(unsigned char reg) {
    outb(0x70, (unsigned char)(reg | 0x80));   // NMI masked
    return inb(0x71);
}

static unsigned int bcd2bin(unsigned char v) {
    return (unsigned int)((v >> 4) * 10 + (v & 0xF));
}

int rtc_get(struct aos_time *t) {
    unsigned char stb = rtc_read(0x0B);
    int binary = (stb & 0x04) != 0;            // status B bit 2
    unsigned int guard = 100000;
    while (rtc_read(0x0A) & 0x80) {            // status A bit 7 = UIP
        if (--guard == 0) return -1;
    }
    unsigned char sec = rtc_read(0x00);
    unsigned char min = rtc_read(0x02);
    unsigned char hr  = rtc_read(0x04);
    unsigned char day = rtc_read(0x07);
    unsigned char mon = rtc_read(0x08);
    unsigned char yr  = rtc_read(0x09);
    unsigned char cen = rtc_read(0x32);
    if (!binary) {
        sec = (unsigned char)bcd2bin(sec);
        min = (unsigned char)bcd2bin(min);
        hr  = (unsigned char)bcd2bin(hr);
        day = (unsigned char)bcd2bin(day);
        mon = (unsigned char)bcd2bin(mon);
        yr  = (unsigned char)bcd2bin(yr);
        cen = (unsigned char)bcd2bin(cen);
    }
    // The century register (CMOS 0x32) is unreliable: QEMU may leave it as
    // garbage (e.g. 0x32), so only trust it if the resulting year is sane.
    int year = 2000 + (int)yr;
    if (cen >= 20 && cen <= 99) {
        int y = (int)cen * 100 + (int)yr;
        if (y >= 1970 && y <= 2100) year = y;
    }
    t->year = year; t->month = (int)mon; t->day = (int)day;
    t->hour = (int)hr; t->minute = (int)min; t->second = (int)sec;
    return 0;
}
