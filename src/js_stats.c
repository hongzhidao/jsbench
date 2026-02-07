#include "js_main.h"

void js_hist_init(js_hist_t *h) {
    memset(h, 0, sizeof(*h));
    h->min_val = 1e18;
    h->max_val = 0;
}

static int us_to_slot(double us) {
    if (us < 0) return 0;
    if (us < HIST_FINE_MAX_US) {
        /* Fine range: 0-10ms at 1us resolution */
        return (int)us;
    }
    /* Coarse range: 10ms-1s at 100us resolution */
    int coarse = (int)((us - HIST_FINE_MAX_US) / HIST_COARSE_STEP);
    if (coarse >= HIST_COARSE_SLOTS) return -1; /* over 1s */
    return HIST_FINE_SLOTS + coarse;
}

static double slot_to_us(int slot) {
    if (slot < HIST_FINE_SLOTS)
        return (double)slot;
    return HIST_FINE_MAX_US + (double)(slot - HIST_FINE_SLOTS) * HIST_COARSE_STEP;
}

void js_hist_add(js_hist_t *h, double us) {
    h->count++;
    h->sum += us;
    h->sum_sq += us * us;
    if (us < h->min_val) h->min_val = us;
    if (us > h->max_val) h->max_val = us;

    int slot = us_to_slot(us);
    if (slot < 0)
        h->over++;
    else
        h->slots[slot]++;
}

void js_hist_merge(js_hist_t *dst, const js_hist_t *src) {
    for (int i = 0; i < HIST_TOTAL_SLOTS; i++)
        dst->slots[i] += src->slots[i];
    dst->over += src->over;
    dst->count += src->count;
    dst->sum += src->sum;
    dst->sum_sq += src->sum_sq;
    if (src->min_val < dst->min_val) dst->min_val = src->min_val;
    if (src->max_val > dst->max_val) dst->max_val = src->max_val;
}

double js_hist_percentile(const js_hist_t *h, double p) {
    if (h->count == 0) return 0;
    uint64_t target = (uint64_t)((double)h->count * p / 100.0);
    uint64_t cumulative = 0;

    for (int i = 0; i < HIST_TOTAL_SLOTS; i++) {
        cumulative += h->slots[i];
        if (cumulative > target)
            return slot_to_us(i);
    }
    return h->max_val;
}

double js_hist_mean(const js_hist_t *h) {
    if (h->count == 0) return 0;
    return h->sum / (double)h->count;
}

double js_hist_stdev(const js_hist_t *h) {
    if (h->count < 2) return 0;
    double mean = h->sum / (double)h->count;
    double variance = (h->sum_sq / (double)h->count) - (mean * mean);
    return variance > 0 ? sqrt(variance) : 0;
}

void js_stats_init(js_stats_t *s) {
    memset(s, 0, sizeof(*s));
    js_hist_init(&s->latency);
}

void js_stats_merge(js_stats_t *dst, const js_stats_t *src) {
    dst->requests += src->requests;
    dst->bytes_read += src->bytes_read;
    dst->errors += src->errors;
    dst->connect_errors += src->connect_errors;
    dst->read_errors += src->read_errors;
    dst->write_errors += src->write_errors;
    dst->timeout_errors += src->timeout_errors;
    dst->status_2xx += src->status_2xx;
    dst->status_3xx += src->status_3xx;
    dst->status_4xx += src->status_4xx;
    dst->status_5xx += src->status_5xx;
    js_hist_merge(&dst->latency, &src->latency);
}

void js_stats_print(const js_stats_t *s, double duration_sec) {
    char bytes_buf[32];
    char min_buf[32], avg_buf[32], max_buf[32], stdev_buf[32];
    char p50_buf[32], p90_buf[32], p99_buf[32], p999_buf[32];

    js_format_bytes(s->bytes_read, bytes_buf, sizeof(bytes_buf));

    double mean = js_hist_mean(&s->latency);
    double stdev = js_hist_stdev(&s->latency);

    js_format_duration(s->latency.min_val, min_buf, sizeof(min_buf));
    js_format_duration(mean, avg_buf, sizeof(avg_buf));
    js_format_duration(s->latency.max_val, max_buf, sizeof(max_buf));
    js_format_duration(stdev, stdev_buf, sizeof(stdev_buf));

    js_format_duration(js_hist_percentile(&s->latency, 50), p50_buf, sizeof(p50_buf));
    js_format_duration(js_hist_percentile(&s->latency, 90), p90_buf, sizeof(p90_buf));
    js_format_duration(js_hist_percentile(&s->latency, 99), p99_buf, sizeof(p99_buf));
    js_format_duration(js_hist_percentile(&s->latency, 99.9), p999_buf, sizeof(p999_buf));

    double qps = duration_sec > 0 ? (double)s->requests / duration_sec : 0;

    printf("\n");
    printf("  requests:  %lu\n", (unsigned long)s->requests);
    printf("  duration:  %.2fs\n", duration_sec);
    printf("  bytes:     %s\n", bytes_buf);
    printf("  errors:    %lu\n", (unsigned long)s->errors);
    printf("  qps:       %.1f\n", qps);
    printf("\n");
    printf("  latency    min       avg       max       stdev\n");
    printf("             %-10s%-10s%-10s%-10s\n", min_buf, avg_buf, max_buf, stdev_buf);
    printf("\n");
    printf("  percentile p50       p90       p99       p999\n");
    printf("             %-10s%-10s%-10s%-10s\n", p50_buf, p90_buf, p99_buf, p999_buf);
    printf("\n");
    printf("  status     2xx       3xx       4xx       5xx\n");
    printf("             %-10lu%-10lu%-10lu%-10lu\n",
           (unsigned long)s->status_2xx, (unsigned long)s->status_3xx,
           (unsigned long)s->status_4xx, (unsigned long)s->status_5xx);
    printf("\n");
}
