#ifndef JS_HTTP_H
#define JS_HTTP_H

#define JS_MAX_HEADERS  64

/* ── HTTP response parser ─────────────────────────────────────────────── */

typedef enum {
    HTTP_PARSE_STATUS_LINE,
    HTTP_PARSE_HEADER_LINE,
    HTTP_PARSE_BODY_IDENTITY,
    HTTP_PARSE_BODY_CHUNKED,
    HTTP_PARSE_CHUNK_SIZE,
    HTTP_PARSE_CHUNK_DATA,
    HTTP_PARSE_CHUNK_TRAILER,
    HTTP_PARSE_DONE,
    HTTP_PARSE_ERROR
} http_parse_state_t;

typedef struct {
    char    name[128];
    char    value[4096];
} js_header_t;

typedef struct {
    http_parse_state_t state;
    int                status_code;
    char               status_text[64];

    js_header_t       headers[JS_MAX_HEADERS];
    int                header_count;

    /* Body handling */
    char              *body;
    size_t             body_len;
    size_t             body_cap;
    size_t             content_length;
    bool               chunked;

    /* Chunk parsing state */
    size_t             chunk_remaining;

    /* Internal parse buffer */
    char              *buf;
    size_t             buf_len;
    size_t             buf_cap;
} js_http_response_t;

/* ── HTTP peer: per-connection HTTP state ─────────────────────────────── */

typedef struct {
    js_http_response_t  response;
    uint64_t            start_ns;
} js_http_peer_t;

void        js_http_response_init(js_http_response_t *r);
void        js_http_response_free(js_http_response_t *r);
void        js_http_response_reset(js_http_response_t *r);
int         js_http_response_feed(js_http_response_t *r, const char *data, size_t len);
const char *js_http_response_header(const js_http_response_t *r, const char *name);

#endif /* JS_HTTP_H */
