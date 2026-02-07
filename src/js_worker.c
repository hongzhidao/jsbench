#include "js_main.h"

/* ── C-path worker: string/object/array modes ─────────────────────────── */

static void worker_c_path(js_worker_t *w) {
    js_config_t *cfg = w->config;
    int epfd = js_epoll_create();
    if (epfd < 0) return;

    /* Timer for duration */
    int tfd = -1;
    if (cfg->duration_sec > 0) {
        tfd = js_timerfd_create(cfg->duration_sec);
        if (tfd >= 0) js_epoll_add(epfd, tfd, EPOLLIN, NULL);
    }

    /* Create connections */
    js_conn_t **conns = calloc((size_t)w->conn_count, sizeof(js_conn_t *));
    int active = 0;

    for (int i = 0; i < w->conn_count; i++) {
        conns[i] = js_conn_create((struct sockaddr *)&cfg->addr, cfg->addr_len,
                                    cfg->use_tls ? cfg->ssl_ctx : NULL,
                                    cfg->requests[0].url.host);
        if (!conns[i]) {
            w->stats.connect_errors++;
            w->stats.errors++;
            continue;
        }

        /* Assign request (round-robin for array mode) */
        int req_idx = i % cfg->request_count;
        conns[i]->req_index = req_idx;
        js_conn_set_request(conns[i], cfg->requests[req_idx].data,
                             cfg->requests[req_idx].len);

        js_epoll_add(epfd, conns[i]->fd, EPOLLIN | EPOLLOUT | EPOLLET, conns[i]);
        active++;
    }

    /* Event loop */
    struct epoll_event events[256];
    bool timer_expired = false;

    while (!timer_expired && !atomic_load(&w->stop) && active > 0) {
        int n = epoll_wait(epfd, events, 256, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                /* Timer fired */
                uint64_t expirations;
                (void)!read(tfd, &expirations, sizeof(expirations));
                timer_expired = true;
                break;
            }

            js_conn_t *c = events[i].data.ptr;
            js_conn_handle_event(c, events[i].events);

            if (c->state == CONN_DONE) {
                /* Record stats */
                uint64_t elapsed_ns = js_now_ns() - c->start_ns;
                double elapsed_us = (double)elapsed_ns / 1000.0;

                w->stats.requests++;
                w->stats.bytes_read += c->response.body_len;
                js_hist_add(&w->stats.latency, elapsed_us);

                int code = c->response.status_code;
                if (code >= 200 && code < 300) w->stats.status_2xx++;
                else if (code >= 300 && code < 400) w->stats.status_3xx++;
                else if (code >= 400 && code < 500) w->stats.status_4xx++;
                else if (code >= 500) w->stats.status_5xx++;

                if (timer_expired || atomic_load(&w->stop)) continue;

                int next_idx = (c->req_index + 1) % cfg->request_count;
                c->req_index = next_idx;

                if (js_conn_keepalive(c)) {
                    /* Reuse connection: just reset parser, send next request */
                    js_conn_reuse(c);
                    js_conn_set_request(c, cfg->requests[next_idx].data,
                                         cfg->requests[next_idx].len);
                    js_epoll_mod(epfd, c->fd, EPOLLIN | EPOLLOUT | EPOLLET, c);
                } else {
                    /* Server closed: reconnect */
                    js_epoll_del(epfd, c->fd);
                    js_conn_reset(c, (struct sockaddr *)&cfg->addr, cfg->addr_len,
                                  cfg->use_tls ? cfg->ssl_ctx : NULL,
                                  cfg->requests[0].url.host);

                    if (c->state == CONN_ERROR) {
                        w->stats.connect_errors++;
                        w->stats.errors++;
                        continue;
                    }

                    js_conn_set_request(c, cfg->requests[next_idx].data,
                                         cfg->requests[next_idx].len);
                    js_epoll_add(epfd, c->fd, EPOLLIN | EPOLLOUT | EPOLLET, c);
                }

            } else if (c->state == CONN_ERROR) {
                w->stats.errors++;
                w->stats.connect_errors++;

                if (timer_expired || atomic_load(&w->stop)) continue;

                /* Reconnect */
                js_epoll_del(epfd, c->fd);

                int next_idx = c->req_index;
                js_conn_reset(c, (struct sockaddr *)&cfg->addr, cfg->addr_len,
                              cfg->use_tls ? cfg->ssl_ctx : NULL,
                              cfg->requests[0].url.host);

                if (c->state == CONN_ERROR) {
                    w->stats.connect_errors++;
                    w->stats.errors++;
                    continue;
                }

                js_conn_set_request(c, cfg->requests[next_idx].data,
                                     cfg->requests[next_idx].len);
                js_epoll_add(epfd, c->fd, EPOLLIN | EPOLLOUT | EPOLLET, c);
            } else {
                /* Still in progress, update epoll interest */
                uint32_t ev = EPOLLET;
                if (c->state == CONN_CONNECTING || c->state == CONN_WRITING ||
                    c->state == CONN_TLS_HANDSHAKE)
                    ev |= EPOLLOUT | EPOLLIN;
                else
                    ev |= EPOLLIN;
                js_epoll_mod(epfd, c->fd, ev, c);
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < w->conn_count; i++) {
        if (conns[i]) js_conn_free(conns[i]);
    }
    free(conns);
    if (tfd >= 0) close(tfd);
    close(epfd);
}

/* ── JS-path worker: async function mode ──────────────────────────────── */

static void worker_js_path(js_worker_t *w) {
    js_config_t *cfg = w->config;

    /* Each JS worker gets its own runtime */
    JSRuntime *rt = js_vm_rt_create();
    JSContext *ctx = js_vm_ctx_create(rt);

    /* Create event loop for fetch() */
    js_loop_t *loop = js_loop_create();
    if (!loop) {
        fprintf(stderr, "Worker %d: failed to create event loop\n", w->id);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return;
    }
    JS_SetContextOpaque(ctx, loop);

    /* Re-evaluate the script to get the async function */
    JSValue default_export = JS_UNDEFINED;
    JSValue bench_export = JS_UNDEFINED;

    if (js_vm_eval_module(ctx, cfg->script_path, cfg->script_source,
                           &default_export, &bench_export) != 0) {
        fprintf(stderr, "Worker %d: failed to evaluate script\n", w->id);
        JS_SetContextOpaque(ctx, NULL);
        js_loop_free(loop);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return;
    }

    if (!JS_IsFunction(ctx, default_export)) {
        fprintf(stderr, "Worker %d: default export is not a function\n", w->id);
        JS_FreeValue(ctx, default_export);
        JS_FreeValue(ctx, bench_export);
        JS_SetContextOpaque(ctx, NULL);
        js_loop_free(loop);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return;
    }

    /* Timer-based duration */
    uint64_t deadline_ns = 0;
    if (cfg->duration_sec > 0) {
        deadline_ns = js_now_ns() + (uint64_t)(cfg->duration_sec * 1e9);
    }

    /* Run the async function in a loop */
    while (!atomic_load(&w->stop)) {
        if (deadline_ns > 0 && js_now_ns() >= deadline_ns) break;

        uint64_t start = js_now_ns();

        /* Call the async function */
        JSValue promise = JS_Call(ctx, default_export, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(promise)) {
            w->stats.errors++;
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, promise);
            continue;
        }

        /* Drive the event loop to resolve pending fetches */
        js_had_unhandled_rejection = 0;
        int rc = js_loop_run(loop, rt);

        JS_FreeValue(ctx, promise);

        uint64_t elapsed_ns = js_now_ns() - start;
        double elapsed_us = (double)elapsed_ns / 1000.0;

        w->stats.requests++;
        js_hist_add(&w->stats.latency, elapsed_us);

        if (rc != 0) {
            w->stats.errors++;
        } else {
            w->stats.status_2xx++;
        }
    }

    JS_FreeValue(ctx, default_export);
    JS_FreeValue(ctx, bench_export);
    JS_SetContextOpaque(ctx, NULL);
    js_loop_free(loop);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* ── Worker thread entry point ────────────────────────────────────────── */

void *js_worker_run(void *arg) {
    js_worker_t *w = arg;
    js_stats_init(&w->stats);

    if (w->config->mode == MODE_BENCH_ASYNC) {
        worker_js_path(w);
    } else {
        worker_c_path(w);
    }

    return NULL;
}
