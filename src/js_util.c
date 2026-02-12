#include "js_main.h"

double js_parse_duration(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    double val = strtod(s, &end);
    if (end == s) return 0;

    switch (*end) {
        case 's': case 'S': case '\0':
            return val;
        case 'm': case 'M':
            if (end[1] == 's' || end[1] == 'S')
                return val / 1000.0;   /* milliseconds */
            return val * 60.0;         /* minutes */
        case 'h': case 'H':
            return val * 3600.0;
        default:
            return val;
    }
}

char *js_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

void js_format_bytes(uint64_t bytes, char *buf, size_t buf_len) {
    if (bytes >= 1073741824ULL)
        snprintf(buf, buf_len, "%.1f GB", (double)bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        snprintf(buf, buf_len, "%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        snprintf(buf, buf_len, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, buf_len, "%lu B", (unsigned long)bytes);
}

void js_format_duration(double us, char *buf, size_t buf_len) {
    if (us >= 1000000.0)
        snprintf(buf, buf_len, "%.2fs", us / 1000000.0);
    else if (us >= 1000.0)
        snprintf(buf, buf_len, "%.2fms", us / 1000.0);
    else
        snprintf(buf, buf_len, "%.2fus", us);
}

int js_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

uint64_t js_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

