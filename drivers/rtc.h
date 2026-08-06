#ifndef RTC_H
#define RTC_H

#include "aosabi.h"    // struct aos_time (single source of truth)

int rtc_get(struct aos_time *t);
void rtc_set_tz(int minutes);
unsigned int rtc_epoch(struct aos_time *t);

#endif