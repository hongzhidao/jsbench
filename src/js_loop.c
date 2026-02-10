#include "js_main.h"

/* ── Pending fetch operation ─────────────────────────────────────────── */

typedef struct {
    js_conn_t   *conn;
    char        *raw_data;      /* serialized HTTP request (conn->req_data points here) */
    SSL_CTX     *ssl_ctx;       /* TLS context, NULL for plain HTTP */
    JSContext   *ctx;
    JSValue      resolve;
    JSValue      reject;
    uint64_t     deadline_ns;
    js_loop_t   *loop;
} js_pending_t;

/* ── Loop structure (opaque to other modules) ────────────────────────── */

struct js_loop {
    js_engine_t   *engine;
    js_pending_t  **items;
    int             count;
    int             cap;
};

#define LOOP_INIT_CAP  16

/* ── Create / free ───────────────────────────────────────────────────── */

js_loop_t *js_loop_create(void) {
    js_loop_t *loop = calloc(1, sizeof(js_loop_t));
    if (!loop) return NULL;

    loop->engine = js_engine_create();
    if (loop->engine == NULL) {
        free(loop);
        return NULL;
    }

    loop->items = calloc(LOOP_INIT_CAP, sizeof(js_pending_t *));
    loop->cap = LOOP_INIT_CAP;
    loop->count = 0;
    return loop;
}

void js_loop_free(js_loop_t *loop) {
    if (!loop) return;

    /* Clean up any remaining pending operations */
    for (int i = 0; i < loop->count; i++) {
        js_pending_t *p = loop->items[i];
        js_conn_free(p->conn);
        free(p->raw_data);
        if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
        JS_FreeValue(p->ctx, p->resolve);
        JS_FreeValue(p->ctx, p->reject);
        free(p);
    }

    free(loop->items);
    js_engine_destroy(loop->engine);
    free(loop);
}

/* ── Internal: remove pending at index (swap with last) ──────────────── */

static void loop_remove(js_loop_t *loop, int idx) {
    loop->items[idx] = loop->items[loop->count - 1];
    loop->items[loop->count - 1] = NULL;
    loop->count--;
}

/* ── Internal: resolve a completed fetch ─────────────────────────────── */

static void pending_complete(js_loop_t *loop, js_pending_t *p, int idx) {
    js_conn_t *conn = p->conn;
    JSContext *ctx = p->ctx;

    /* Build Response object and resolve the promise */
    JSValue response = js_response_new(ctx,
        conn->response.status_code,
        conn->response.status_text,
        conn->response.body,
        conn->response.body_len,
        &conn->response);

    JSValue ret = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, &response);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, response);

    /* Cleanup */
    js_epoll_del(loop->engine, &conn->socket);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_conn_free(conn);
    free(p->raw_data);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    free(p);

    loop_remove(loop, idx);
}

/* ── Internal: reject a failed/timed-out fetch ───────────────────────── */

static void pending_fail(js_loop_t *loop, js_pending_t *p, int idx,
                         const char *message) {
    JSContext *ctx = p->ctx;

    JSValue err = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, err, "message",
                              JS_NewString(ctx, message),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JSValue ret = JS_Call(ctx, p->reject, JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, err);

    /* Cleanup */
    js_epoll_del(loop->engine, &p->conn->socket);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_conn_free(p->conn);
    free(p->raw_data);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    free(p);

    loop_remove(loop, idx);
}

/* ── Loop connection handlers ────────────────────────────────────────── */

static void loop_conn_process(js_conn_t *c) {
    js_pending_t *p = c->udata;
    js_loop_t *loop = p->loop;

    if (c->state == CONN_DONE) {
        for (int i = 0; i < loop->count; i++) {
            if (loop->items[i] == p) {
                pending_complete(loop, p, i);
                return;
            }
        }
    } else if (c->state == CONN_ERROR) {
        for (int i = 0; i < loop->count; i++) {
            if (loop->items[i] == p) {
                pending_fail(loop, p, i, "Connection error");
                return;
            }
        }
    } else {
        /* Update epoll interest based on connection state */
        uint32_t mask = EPOLLET;
        if (c->state == CONN_CONNECTING || c->state == CONN_WRITING ||
            c->state == CONN_TLS_HANDSHAKE)
            mask |= EPOLLOUT | EPOLLIN;
        else
            mask |= EPOLLIN;
        js_epoll_mod(loop->engine, &c->socket, mask);
    }
}

static void loop_on_read(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    int rc = js_conn_read(c);

    if (c->state == CONN_READING && c->in.len > 0) {
        int ret = js_http_response_feed(&c->response, c->in.data, c->in.len);
        js_buf_reset(&c->in);

        if (ret == 1) {
            c->state = CONN_DONE;
        } else if (ret < 0) {
            c->state = CONN_ERROR;
        }
    }

    if (rc == 1 && c->state == CONN_READING) {
        /* Peer closed before HTTP response complete */
        if (c->response.state == HTTP_PARSE_BODY_IDENTITY ||
            c->response.body_len > 0) {
            c->state = CONN_DONE;
        } else {
            c->state = CONN_ERROR;
        }
    }

    loop_conn_process(c);
}

static void loop_on_write(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    js_conn_process_write(c);
    loop_conn_process(c);
}

static void loop_on_error(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    c->state = CONN_ERROR;
    loop_conn_process(c);
}

/* ── Add a pending fetch ─────────────────────────────────────────────── */

int js_loop_add(js_loop_t *loop, js_conn_t *conn, char *raw_data,
                SSL_CTX *ssl_ctx, JSContext *ctx,
                JSValue resolve, JSValue reject) {
    /* Grow array if needed */
    if (loop->count >= loop->cap) {
        int new_cap = loop->cap * 2;
        js_pending_t **new_items = realloc(loop->items,
                                           sizeof(js_pending_t *) * (size_t)new_cap);
        if (!new_items) return -1;
        loop->items = new_items;
        loop->cap = new_cap;
    }

    js_pending_t *p = calloc(1, sizeof(js_pending_t));
    if (!p) return -1;

    p->conn = conn;
    p->raw_data = raw_data;
    p->ssl_ctx = ssl_ctx;
    p->ctx = ctx;
    p->resolve = resolve;
    p->reject = reject;
    p->deadline_ns = js_now_ns() + (uint64_t)(30.0 * 1e9);  /* 30s timeout */
    p->loop = loop;

    conn->udata = p;
    conn->socket.read  = loop_on_read;
    conn->socket.write = loop_on_write;
    conn->socket.error = loop_on_error;

    js_epoll_add(loop->engine, &conn->socket, EPOLLIN | EPOLLOUT | EPOLLET);

    loop->items[loop->count++] = p;
    return 0;
}

int js_loop_pending(js_loop_t *loop) {
    return loop->count;
}

/* ── Run the event loop ──────────────────────────────────────────────── */

int js_loop_run(js_loop_t *loop, JSRuntime *rt) {
    JSContext *pctx;

    for (;;) {
        /* 1. Drain all pending JS jobs */
        int js_ret;
        do {
            js_ret = JS_ExecutePendingJob(rt, &pctx);
        } while (js_ret > 0);

        if (js_ret < 0) {
            /* JS exception */
            JSValue exc = JS_GetException(pctx);
            const char *str = JS_ToCString(pctx, exc);
            if (str) {
                fprintf(stderr, "Error: %s\n", str);
                JS_FreeCString(pctx, str);
            }
            JS_FreeValue(pctx, exc);
            return 1;
        }

        /* 2. If no pending I/O, we're done */
        if (loop->count == 0) break;

        /* 3. Poll for events and dispatch through handlers */
        if (js_epoll_poll(loop->engine, 100) < 0) break;

        /* 4. Check timeouts */
        uint64_t now = js_now_ns();
        for (int i = loop->count - 1; i >= 0; i--) {
            if (now >= loop->items[i]->deadline_ns) {
                pending_fail(loop, loop->items[i], i, "Request timeout");
            }
        }
    }

    /* Check for unhandled promise rejections */
    if (js_had_unhandled_rejection)
        return 1;

    return 0;
}
