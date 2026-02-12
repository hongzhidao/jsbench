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
