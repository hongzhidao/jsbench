#ifndef JS_BUF_H
#define JS_BUF_H

typedef struct {
    char   *data;
    size_t  len;     /* bytes of valid data */
    size_t  cap;     /* allocated capacity */
    size_t  pos;     /* current position (bytes consumed/sent) */
} js_buf_t;

static inline void js_buf_init(js_buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static inline void js_buf_free(js_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->pos = 0;
}

static inline void js_buf_reset(js_buf_t *b) {
    b->len = 0;
    b->pos = 0;
}

static inline int js_buf_ensure(js_buf_t *b, size_t need) {
    if (b->cap >= need) return 0;

    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) cap *= 2;

    char *data = realloc(b->data, cap);
    if (!data) return -1;

    b->data = data;
    b->cap = cap;
    return 0;
}

#endif /* JS_BUF_H */
