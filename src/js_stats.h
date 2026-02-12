#ifndef JS_STATS_H
#define JS_STATS_H

/* Histogram: 0-10ms at 1us resolution, 10ms-1s at 100us resolution */
#define HIST_FINE_SLOTS      10000   /* 0..9999 us  (0-10ms) */
#define HIST_COARSE_SLOTS    9900    /* 10000..999900 us (10ms-1s) */
#define HIST_TOTAL_SLOTS     (HIST_FINE_SLOTS + HIST_COARSE_SLOTS)
#define HIST_FINE_MAX_US     10000
#define HIST_COARSE_STEP     100

/* ── Histogram ────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t  slots[HIST_TOTAL_SLOTS];
    uint64_t  over;          /* > 1s */
    uint64_t  count;
    double    sum;           /* sum of all latencies in us */
    double    sum_sq;        /* sum of squares */
    double    min_val;
    double    max_val;
} js_hist_t;

/* ── Per-worker stats ─────────────────────────────────────────────────── */

typedef struct {
    uint64_t   requests;
    uint64_t   bytes_read;
    uint64_t   errors;
    uint64_t   connect_errors;
    uint64_t   read_errors;
    uint64_t   write_errors;
    uint64_t   timeout_errors;
    uint64_t   status_2xx;
    uint64_t   status_3xx;
    uint64_t   status_4xx;
    uint64_t   status_5xx;
    js_hist_t latency;
} js_stats_t;

void    js_hist_init(js_hist_t *h);
void    js_hist_add(js_hist_t *h, double us);
void    js_hist_merge(js_hist_t *dst, const js_hist_t *src);
double  js_hist_percentile(const js_hist_t *h, double p);
double  js_hist_mean(const js_hist_t *h);
double  js_hist_stdev(const js_hist_t *h);
void    js_stats_init(js_stats_t *s);
void    js_stats_merge(js_stats_t *dst, const js_stats_t *src);
void    js_stats_print(const js_stats_t *s, double duration_sec);

#endif /* JS_STATS_H */
