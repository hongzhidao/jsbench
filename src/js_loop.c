#include "js_main.h"

/* ── Loop structure (opaque to other modules) ────────────────────────── */

struct js_loop {
    struct list_head  pending;
};

/* ── Create / free ───────────────────────────────────────────────────── */

js_loop_t *js_loop_create(void) {
    js_loop_t *loop = calloc(1, sizeof(js_loop_t));
    if (!loop) return NULL;

    init_list_head(&loop->pending);
    return loop;
}

void js_loop_free(js_loop_t *loop) {
    if (!loop) return;

    /* Clean up any remaining pending operations */
    struct list_head *el, *el1;
    list_for_each_safe(el, el1, &loop->pending) {
        js_pending_t *p = list_entry(el, js_pending_t, link);
        js_fetch_t *f = js_fetch_from_pending(p);
        js_timer_delete(&js_thread()->engine->timers, &f->timer);
        js_http_response_free(&f->response);
        js_conn_free(f->conn);
        if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
        JS_FreeValue(p->ctx, p->resolve);
        JS_FreeValue(p->ctx, p->reject);
        list_del(&p->link);
        free(f);
    }

    free(loop);
}

/* ── Internal: resolve a completed fetch ─────────────────────────────── */

static void pending_complete(js_loop_t *loop, js_pending_t *p) {
    js_fetch_t *f = js_fetch_from_pending(p);
    js_conn_t *conn = f->conn;
    js_http_response_t *r = &f->response;
    JSContext *ctx = p->ctx;

    /* Build Response object and resolve the promise */
    JSValue response = js_response_new(ctx,
        r->status_code,
        r->status_text,
        r->body,
        r->body_len,
        r);

    JSValue ret = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, &response);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, response);

    /* Cleanup */
    js_epoll_del(js_thread()->engine, &conn->socket);
    js_timer_delete(&js_thread()->engine->timers, &f->timer);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_http_response_free(r);
    js_conn_free(conn);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    list_del(&p->link);
    free(f);
}

/* ── Internal: reject a failed/timed-out fetch ───────────────────────── */

static void pending_fail(js_loop_t *loop, js_pending_t *p,
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
    js_fetch_t *f = js_fetch_from_pending(p);
    js_epoll_del(js_thread()->engine, &f->conn->socket);
    js_timer_delete(&js_thread()->engine->timers, &f->timer);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_conn_free(f->conn);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    list_del(&p->link);
    free(f);
}

/* ── Loop connection handlers ────────────────────────────────────────── */

static void loop_conn_process(js_conn_t *c) {
    js_fetch_t *f = c->socket.data;
    js_pending_t *p = &f->pending;
    js_loop_t *loop = p->loop;

    if (c->state == CONN_DONE) {
        pending_complete(loop, p);
    } else if (c->state == CONN_ERROR) {
        pending_fail(loop, p, "Connection error");
    } else {
        /* Update epoll interest based on connection state */
        uint32_t mask = EPOLLET;
        if (c->state == CONN_CONNECTING || c->state == CONN_WRITING ||
            c->state == CONN_TLS_HANDSHAKE)
            mask |= EPOLLOUT | EPOLLIN;
        else
            mask |= EPOLLIN;
        js_epoll_mod(js_thread()->engine, &c->socket, mask);
    }
}

static void loop_on_read(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    js_fetch_t *f = c->socket.data;
    js_http_response_t *r = &f->response;
    int rc = js_conn_read(c);

    if (c->state == CONN_READING && c->in.len > 0) {
        int ret = js_http_response_feed(r, c->in.data, c->in.len);
        js_buf_reset(&c->in);

        if (ret == 1) {
            c->state = CONN_DONE;
        } else if (ret < 0) {
            c->state = CONN_ERROR;
        }
    }

    if (rc == 1 && c->state == CONN_READING) {
        /* Peer closed before HTTP response complete */
        if (r->state == HTTP_PARSE_BODY_IDENTITY ||
            r->body_len > 0) {
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

int js_loop_add(js_loop_t *loop, js_pending_t *p) {
    p->loop = loop;

    js_fetch_t *f = js_fetch_from_pending(p);
    js_conn_t *conn = f->conn;
    conn->socket.read  = loop_on_read;
    conn->socket.write = loop_on_write;
    conn->socket.error = loop_on_error;

    js_epoll_add(js_thread()->engine, &conn->socket, EPOLLIN | EPOLLOUT | EPOLLET);

    list_add_tail(&p->link, &loop->pending);
    return 0;
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
        if (list_empty(&loop->pending)) break;

        /* 3. Poll for events and dispatch through handlers */
        js_msec_t timer_timeout = js_timer_find(&js_thread()->engine->timers);
        int timeout = (timer_timeout == (js_msec_t) -1)
                    ? 100 : (int) timer_timeout;

        if (js_epoll_poll(js_thread()->engine, timeout) < 0) break;

        /* 4. Expire timers */
        js_thread()->engine->timers.now = (js_msec_t)(js_now_ns() / 1000000);
        js_timer_expire(&js_thread()->engine->timers, js_thread()->engine->timers.now);
    }

    /* Check for unhandled promise rejections */
    if (js_had_unhandled_rejection)
        return 1;

    return 0;
}
