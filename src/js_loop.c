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
} js_pending_t;

/* ── Loop structure (opaque to other modules) ────────────────────────── */

struct js_loop {
    int             epfd;
    js_pending_t  **items;
    int             count;
    int             cap;
};

#define LOOP_INIT_CAP  16

/* ── Create / free ───────────────────────────────────────────────────── */

js_loop_t *js_loop_create(void) {
    js_loop_t *loop = calloc(1, sizeof(js_loop_t));
    if (!loop) return NULL;

    loop->epfd = js_epoll_create();
    if (loop->epfd < 0) {
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
    if (loop->epfd >= 0) close(loop->epfd);
    free(loop);
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

    conn->udata = p;

    js_epoll_add(loop->epfd, conn->fd, EPOLLIN | EPOLLOUT | EPOLLET, conn);

    loop->items[loop->count++] = p;
    return 0;
}

int js_loop_pending(js_loop_t *loop) {
    return loop->count;
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
    js_epoll_del(loop->epfd, conn->fd);
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
    js_epoll_del(loop->epfd, p->conn->fd);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_conn_free(p->conn);
    free(p->raw_data);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    free(p);

    loop_remove(loop, idx);
}

/* ── Run the event loop ──────────────────────────────────────────────── */

int js_loop_run(js_loop_t *loop, JSRuntime *rt) {
    JSContext *pctx;
    struct epoll_event events[64];

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

        /* 3. Wait for I/O events */
        int n = epoll_wait(loop->epfd, events, 64, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 4. Process events */
        for (int i = 0; i < n; i++) {
            js_conn_t *c = events[i].data.ptr;
            js_conn_handle_event(c, events[i].events);

            js_pending_t *p = c->udata;

            if (c->state == CONN_DONE) {
                /* Find index in items array */
                for (int j = 0; j < loop->count; j++) {
                    if (loop->items[j] == p) {
                        pending_complete(loop, p, j);
                        break;
                    }
                }
            } else if (c->state == CONN_ERROR) {
                for (int j = 0; j < loop->count; j++) {
                    if (loop->items[j] == p) {
                        pending_fail(loop, p, j, "Connection error");
                        break;
                    }
                }
            } else {
                /* Update epoll interest based on connection state */
                uint32_t ev = EPOLLET;
                if (c->state == CONN_CONNECTING || c->state == CONN_WRITING ||
                    c->state == CONN_TLS_HANDSHAKE)
                    ev |= EPOLLOUT | EPOLLIN;
                else
                    ev |= EPOLLIN;
                js_epoll_mod(loop->epfd, c->fd, ev, c);
            }
        }

        /* 5. Check timeouts */
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
