#include "js_main.h"
#include <ctype.h>

void js_http_response_init(js_http_response_t *r) {
    memset(r, 0, sizeof(*r));
    r->state = HTTP_PARSE_STATUS_LINE;
    r->body_cap = 1024;
    r->body = malloc(r->body_cap);
    r->buf_cap = 4096;
    r->buf = malloc(r->buf_cap);
}

void js_http_response_free(js_http_response_t *r) {
    free(r->body);
    free(r->buf);
    r->body = NULL;
    r->buf = NULL;
}

void js_http_response_reset(js_http_response_t *r) {
    r->state = HTTP_PARSE_STATUS_LINE;
    r->status_code = 0;
    r->status_text[0] = '\0';
    r->header_count = 0;
    r->body_len = 0;
    r->content_length = 0;
    r->chunked = false;
    r->chunk_remaining = 0;
    r->buf_len = 0;
}

static void body_append(js_http_response_t *r, const char *data, size_t len) {
    while (r->body_len + len > r->body_cap) {
        r->body_cap *= 2;
        r->body = realloc(r->body, r->body_cap);
    }
    memcpy(r->body + r->body_len, data, len);
    r->body_len += len;
}

static void buf_append(js_http_response_t *r, const char *data, size_t len) {
    while (r->buf_len + len > r->buf_cap) {
        r->buf_cap *= 2;
        r->buf = realloc(r->buf, r->buf_cap);
    }
    memcpy(r->buf + r->buf_len, data, len);
    r->buf_len += len;
}

/* Find \r\n in buffer starting at offset, return index or -1 */
static ssize_t find_crlf(const char *buf, size_t len, size_t offset) {
    for (size_t i = offset; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n')
            return (ssize_t)i;
    }
    return -1;
}

static int parse_status_line(js_http_response_t *r) {
    ssize_t pos = find_crlf(r->buf, r->buf_len, 0);
    if (pos < 0) return 0; /* need more data */

    /* "HTTP/1.1 200 OK" */
    char *line = r->buf;
    line[pos] = '\0';

    if (strncmp(line, "HTTP/1.", 7) != 0) {
        r->state = HTTP_PARSE_ERROR;
        return -1;
    }

    char *sp1 = strchr(line + 7, ' ');
    if (!sp1) { r->state = HTTP_PARSE_ERROR; return -1; }
    sp1++;

    r->status_code = atoi(sp1);
    char *sp2 = strchr(sp1, ' ');
    if (sp2) {
        sp2++;
        size_t tlen = strlen(sp2);
        if (tlen >= sizeof(r->status_text)) tlen = sizeof(r->status_text) - 1;
        memcpy(r->status_text, sp2, tlen);
        r->status_text[tlen] = '\0';
    }

    /* Consume the line + \r\n */
    size_t consumed = (size_t)pos + 2;
    memmove(r->buf, r->buf + consumed, r->buf_len - consumed);
    r->buf_len -= consumed;

    r->state = HTTP_PARSE_HEADER_LINE;
    return 1;
}

static int parse_header_line(js_http_response_t *r) {
    ssize_t pos = find_crlf(r->buf, r->buf_len, 0);
    if (pos < 0) return 0; /* need more data */

    if (pos == 0) {
        /* Empty line = end of headers */
        size_t consumed = 2;
        memmove(r->buf, r->buf + consumed, r->buf_len - consumed);
        r->buf_len -= consumed;

        /* Determine body mode */
        const char *te = js_http_response_header(r, "Transfer-Encoding");
        if (te && strcasecmp(te, "chunked") == 0) {
            r->chunked = true;
            r->state = HTTP_PARSE_CHUNK_SIZE;
        } else {
            const char *cl = js_http_response_header(r, "Content-Length");
            if (cl) {
                r->content_length = (size_t)atol(cl);
                if (r->content_length == 0) {
                    r->state = HTTP_PARSE_DONE;
                } else {
                    r->state = HTTP_PARSE_BODY_IDENTITY;
                }
            } else {
                /* No content-length, no chunked — assume no body for now */
                r->state = HTTP_PARSE_DONE;
            }
        }
        return 1;
    }

    /* Parse "Name: Value" */
    r->buf[pos] = '\0';
    char *colon = strchr(r->buf, ':');
    if (!colon) {
        /* Skip malformed header */
        size_t consumed = (size_t)pos + 2;
        memmove(r->buf, r->buf + consumed, r->buf_len - consumed);
        r->buf_len -= consumed;
        return 1;
    }

    if (r->header_count < JS_MAX_HEADERS) {
        js_header_t *h = &r->headers[r->header_count];
        size_t nlen = (size_t)(colon - r->buf);
        if (nlen >= sizeof(h->name)) nlen = sizeof(h->name) - 1;
        memcpy(h->name, r->buf, nlen);
        h->name[nlen] = '\0';

        /* Skip ": " */
        char *val = colon + 1;
        while (*val == ' ') val++;
        size_t vlen = strlen(val);
        if (vlen >= sizeof(h->value)) vlen = sizeof(h->value) - 1;
        memcpy(h->value, val, vlen);
        h->value[vlen] = '\0';

        r->header_count++;
    }

    size_t consumed = (size_t)pos + 2;
    memmove(r->buf, r->buf + consumed, r->buf_len - consumed);
    r->buf_len -= consumed;
    return 1;
}

static int parse_body_identity(js_http_response_t *r) {
    size_t remaining = r->content_length - r->body_len;
    size_t avail = r->buf_len < remaining ? r->buf_len : remaining;

    if (avail > 0) {
        body_append(r, r->buf, avail);
        memmove(r->buf, r->buf + avail, r->buf_len - avail);
        r->buf_len -= avail;
    }

    if (r->body_len >= r->content_length) {
        r->state = HTTP_PARSE_DONE;
        return 1;
    }
    return 0;
}

static int parse_chunk_size(js_http_response_t *r) {
    ssize_t pos = find_crlf(r->buf, r->buf_len, 0);
    if (pos < 0) return 0;

    r->buf[pos] = '\0';
    char *end;
    unsigned long chunk_size = strtoul(r->buf, &end, 16);

    size_t consumed = (size_t)pos + 2;
    memmove(r->buf, r->buf + consumed, r->buf_len - consumed);
    r->buf_len -= consumed;

    if (chunk_size == 0) {
        r->state = HTTP_PARSE_CHUNK_TRAILER;
        return 1;
    }

    r->chunk_remaining = chunk_size;
    r->state = HTTP_PARSE_CHUNK_DATA;
    return 1;
}

static int parse_chunk_data(js_http_response_t *r) {
    size_t avail = r->buf_len < r->chunk_remaining ? r->buf_len : r->chunk_remaining;

    if (avail > 0) {
        body_append(r, r->buf, avail);
        memmove(r->buf, r->buf + avail, r->buf_len - avail);
        r->buf_len -= avail;
        r->chunk_remaining -= avail;
    }

    if (r->chunk_remaining == 0) {
        /* Expect \r\n after chunk data */
        if (r->buf_len >= 2) {
            memmove(r->buf, r->buf + 2, r->buf_len - 2);
            r->buf_len -= 2;
            r->state = HTTP_PARSE_CHUNK_SIZE;
            return 1;
        }
        return 0;
    }
    return 0;
}

static int parse_chunk_trailer(js_http_response_t *r) {
    /* Read trailing \r\n */
    ssize_t pos = find_crlf(r->buf, r->buf_len, 0);
    if (pos < 0) {
        /* If buffer is empty, we may already be past the trailer */
        if (r->buf_len == 0) {
            r->state = HTTP_PARSE_DONE;
            return 1;
        }
        return 0;
    }

    size_t consumed = (size_t)pos + 2;
    memmove(r->buf, r->buf + consumed, r->buf_len - consumed);
    r->buf_len -= consumed;

    if (pos == 0) {
        /* Empty line = end of chunked body */
        r->state = HTTP_PARSE_DONE;
        return 1;
    }
    /* Non-empty trailer line, keep reading trailers */
    return 1;
}

int js_http_response_feed(js_http_response_t *r, const char *data, size_t len) {
    buf_append(r, data, len);

    int progress = 1;
    while (progress && r->state != HTTP_PARSE_DONE && r->state != HTTP_PARSE_ERROR) {
        switch (r->state) {
            case HTTP_PARSE_STATUS_LINE:
                progress = parse_status_line(r);
                break;
            case HTTP_PARSE_HEADER_LINE:
                progress = parse_header_line(r);
                break;
            case HTTP_PARSE_BODY_IDENTITY:
                progress = parse_body_identity(r);
                break;
            case HTTP_PARSE_CHUNK_SIZE:
                progress = parse_chunk_size(r);
                break;
            case HTTP_PARSE_CHUNK_DATA:
                progress = parse_chunk_data(r);
                break;
            case HTTP_PARSE_CHUNK_TRAILER:
                progress = parse_chunk_trailer(r);
                break;
            default:
                progress = 0;
                break;
        }
        if (progress < 0) return -1;
    }

    if (r->state == HTTP_PARSE_DONE) return 1;
    if (r->state == HTTP_PARSE_ERROR) return -1;
    return 0;  /* need more data */
}

const char *js_http_response_header(const js_http_response_t *r, const char *name) {
    for (int i = 0; i < r->header_count; i++) {
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
    }
    return NULL;
}
