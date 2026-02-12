#ifndef JS_UTIL_H
#define JS_UTIL_H

double   js_parse_duration(const char *s);
char    *js_read_file(const char *path, size_t *len);
void     js_format_bytes(uint64_t bytes, char *buf, size_t buf_len);
void     js_format_duration(double us, char *buf, size_t buf_len);
int      js_set_nonblocking(int fd);
uint64_t js_now_ns(void);

#endif /* JS_UTIL_H */
