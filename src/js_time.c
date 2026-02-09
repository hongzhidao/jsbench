#include "js_main.h"

void js_realtime(js_realtime_t *now)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_REALTIME, &ts);

    now->sec = (js_time_t) ts.tv_sec;
    now->nsec = ts.tv_nsec;
}

void js_monotonic_time(js_monotonic_time_t *now)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);

    now->monotonic = (js_nsec_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void js_localtime(js_time_t s, struct tm *tm)
{
    time_t _s;

    _s = (time_t) s;
    (void) localtime_r(&_s, tm);
}
