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

    struct list_head *el, *el1;
    list_for_each_safe(el, el1, &loop->pending) {
        js_pending_t *p = list_entry(el, js_pending_t, link);
        js_fetch_destroy(js_fetch_from_pending(p));
    }

    free(loop);
}

/* ── Add a pending fetch ─────────────────────────────────────────────── */

int js_loop_add(js_loop_t *loop, js_pending_t *p) {
    p->loop = loop;

    js_fetch_t *f = js_fetch_from_pending(p);
    js_epoll_add(js_thread()->engine, &f->conn->socket, EPOLLIN | EPOLLOUT | EPOLLET);

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
