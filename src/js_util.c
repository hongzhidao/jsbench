#include "js_main.h"
#include <ctype.h>

int js_parse_url(const char *url_str, js_url_t *out) {
    memset(out, 0, sizeof(*out));

    const char *p = url_str;

    /* Scheme */
    if (strncmp(p, "https://", 8) == 0) {
        strcpy(out->scheme, "https");
        out->is_tls = true;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        strcpy(out->scheme, "http");
        out->is_tls = false;
        out->port = 80;
        p += 7;
    } else {
        return -1;
    }

    /* Host (possibly with port) */
    const char *host_start = p;
    const char *host_end = NULL;
    const char *port_start = NULL;

    /* Find end of host:port section */
    const char *slash = strchr(p, '/');
    const char *section_end = slash ? slash : p + strlen(p);

    /* Check for port */
    const char *colon = NULL;
    for (const char *c = host_start; c < section_end; c++) {
        if (*c == ':') colon = c;
    }

    if (colon) {
        host_end = colon;
        port_start = colon + 1;
        out->port = atoi(port_start);
        size_t port_len = (size_t)(section_end - port_start);
        if (port_len >= sizeof(out->port_str)) return -1;
        memcpy(out->port_str, port_start, port_len);
        out->port_str[port_len] = '\0';
    } else {
        host_end = section_end;
        snprintf(out->port_str, sizeof(out->port_str), "%d", out->port);
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    /* Path */
    if (slash) {
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(out->path)) return -1;
        memcpy(out->path, slash, path_len);
        out->path[path_len] = '\0';
    } else {
        strcpy(out->path, "/");
    }

    return 0;
}

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

int js_request_serialize(js_request_t *req, const char *host_override,
                          js_buf_t *out) {
    const char *method = req->method ? req->method : "GET";
    const char *path = req->url.path[0] ? req->url.path : "/";
    const char *host = host_override ? host_override : req->url.host;

    /* Calculate size */
    size_t cap = strlen(method) + 1 + strlen(path) + 32;   /* request line */
    cap += strlen("Host: ") + strlen(host) + 4;

    bool need_port = (req->url.is_tls && req->url.port != 443) ||
                     (!req->url.is_tls && req->url.port != 80);
    if (need_port && !host_override) cap += 8;

    if (req->headers) cap += strlen(req->headers);
    if (req->body && req->body_len > 0) {
        cap += 64;
        cap += req->body_len;
    }
    cap += 32; /* Connection: keep-alive\r\n */
    cap += 4;  /* final \r\n */

    if (js_buf_ensure(out, cap) < 0) return -1;

    int off = 0;

    off += sprintf(out->data + off, "%s %s HTTP/1.1\r\n", method, path);

    if (need_port && !host_override)
        off += sprintf(out->data + off, "Host: %s:%d\r\n", host, req->url.port);
    else
        off += sprintf(out->data + off, "Host: %s\r\n", host);

    if (req->headers && req->headers[0]) {
        size_t hlen = strlen(req->headers);
        memcpy(out->data + off, req->headers, hlen);
        off += (int)hlen;
        if (hlen < 2 || out->data[off-1] != '\n') {
            out->data[off++] = '\r';
            out->data[off++] = '\n';
        }
    }

    off += sprintf(out->data + off, "Connection: keep-alive\r\n");

    if (req->body && req->body_len > 0)
        off += sprintf(out->data + off, "Content-Length: %zu\r\n", req->body_len);

    out->data[off++] = '\r';
    out->data[off++] = '\n';

    if (req->body && req->body_len > 0) {
        memcpy(out->data + off, req->body, req->body_len);
        off += (int)req->body_len;
    }

    out->len = (size_t)off;
    return 0;
}

void js_request_free(js_request_t *req) {
    free(req->method);
    free(req->headers);
    free(req->body);
}
