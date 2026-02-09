#ifndef JS_TIME_H
#define JS_TIME_H

typedef int64_t js_time_t;
typedef uint32_t js_msec_t;
typedef int32_t js_msec_int_t;
typedef uint64_t js_nsec_t;

typedef struct {
    js_time_t sec;
    uint32_t nsec;
} js_realtime_t;

typedef struct {
    js_realtime_t realtime;
    js_nsec_t monotonic;
    js_nsec_t update;
} js_monotonic_time_t;

void js_realtime(js_realtime_t *now);
void js_monotonic_time(js_monotonic_time_t *now);
void js_localtime(js_time_t s, struct tm *tm);

#endif /* JS_TIME_H */
