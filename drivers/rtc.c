#include "ports.h"
#include "rtc.h"

static int tz_min;

void rtc_set_tz(int minutes) { tz_min = minutes; }

// Howard Hinnant's days-from-civil / civil-from-days (proleptic Gregorian).
static int days_from_civil(int y, unsigned int m, unsigned int d) {
    y -= (int)(m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned int yoe = (unsigned int)(y - era * 400);
    unsigned int doy = (153 * (m + (m > 2 ? 0u : 9u)) + 2) / 5 + d - 1;
    unsigned int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static void civil_from_days(int z, int *y, unsigned int *m, unsigned int *d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned int doe = (unsigned int)(z - era * 146097);
    unsigned int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (int)yoe + era * 400;
    unsigned int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3u : (unsigned int)-9);
    if (*m <= 2) *y += 1;
}

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

    if (tz_min != 0) {
        long total = (long)days_from_civil(year, (unsigned int)mon,
                                           (unsigned int)day) * 86400L +
                     (long)hr * 3600L + (long)min * 60L + (long)sec + tz_min;
        if (total < 0) total = 0;
        int days = (int)(total / 86400L);
        int rem = (int)(total % 86400L);
        hr = (unsigned char)(rem / 3600);
        min = (unsigned char)((rem % 3600) / 60);
        sec = (unsigned char)(rem % 60);
        unsigned int mo, da;
        civil_from_days(days, &year, &mo, &da);
        t->month = (int)mo;
        t->day = (int)da;
    }

    return 0;
}

unsigned int rtc_epoch(struct aos_time *t) {
    long total = (long)days_from_civil(t->year, (unsigned int)t->month,
                                       (unsigned int)t->day) * 86400L +
                 (long)t->hour * 3600L + (long)t->minute * 60L + (long)t->second;
    return total < 0 ? 0u : (unsigned int)total;
}
