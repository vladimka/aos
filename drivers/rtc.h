#ifndef RTC_H
#define RTC_H

struct aos_time {
    int year, month, day, hour, minute, second;
};

int rtc_get(struct aos_time *t);
void rtc_set_tz(int minutes);

#endif
