#include "js_main.h"

/* ── Duration timer handler ──────────────────────────────────────────── */

static void worker_duration_handler(js_timer_t *timer, void *data) {
    atomic_bool *stop = data;
    atomic_store(stop, true);
}

/* ── C-path connection handlers ──────────────────────────────────────── */

static bool worker_keepalive(js_http_response_t *r) {
    const char *conn_hdr = js_http_response_header(r, "Connection");
    if (conn_hdr && strcasecmp(conn_hdr, "close") == 0)
        return false;
    return true;
}

static void worker_conn_process(js_conn_t *c) {
    js_worker_t *w = c->udata;
    js_http_response_t *r = c->socket.data;
    js_config_t *cfg = w->config;
    js_engine_t *engine = w->engine;

    if (c->state == CONN_DONE) {
        /* Record stats */
        uint64_t elapsed_ns = js_now_ns() - c->start_ns;
        double elapsed_us = (double)elapsed_ns / 1000.0;

        w->stats.requests++;
        w->stats.bytes_read += r->body_len;
        js_hist_add(&w->stats.latency, elapsed_us);

        int code = r->status_code;
        if (code >= 200 && code < 300) w->stats.status_2xx++;
        else if (code >= 300 && code < 400) w->stats.status_3xx++;
        else if (code >= 400 && code < 500) w->stats.status_4xx++;
        else if (code >= 500) w->stats.status_5xx++;

        if (atomic_load(&w->stop)) return;

        int next_idx = (c->req_index + 1) % cfg->request_count;
        c->req_index = next_idx;

        if (worker_keepalive(r)) {
            /* Reuse connection: reset parser, send next request */
            js_http_response_reset(r);
            js_conn_reuse(c);
            js_conn_set_request(c, cfg->requests[next_idx].data,
                                 cfg->requests[next_idx].len);
            js_epoll_mod(engine, &c->socket, EPOLLIN | EPOLLOUT | EPOLLET);
        } else {
            /* Server closed: reconnect */
            js_epoll_del(engine, &c->socket);
            js_conn_reset(c, (struct sockaddr *)&cfg->addr, cfg->addr_len,
                          cfg->use_tls ? cfg->ssl_ctx : NULL,
                          cfg->requests[0].url.host);

            if (c->state == CONN_ERROR) {
                w->stats.connect_errors++;
                w->stats.errors++;
                return;
            }

            js_conn_set_request(c, cfg->requests[next_idx].data,
                                 cfg->requests[next_idx].len);
            js_epoll_add(engine, &c->socket, EPOLLIN | EPOLLOUT | EPOLLET);
        }

    } else if (c->state == CONN_ERROR) {
        w->stats.errors++;
        w->stats.connect_errors++;

        if (atomic_load(&w->stop)) return;

        /* Reconnect */
        js_epoll_del(engine, &c->socket);

        int next_idx = c->req_index;
        js_conn_reset(c, (struct sockaddr *)&cfg->addr, cfg->addr_len,
                      cfg->use_tls ? cfg->ssl_ctx : NULL,
                      cfg->requests[0].url.host);

        if (c->state == CONN_ERROR) {
            w->stats.connect_errors++;
            w->stats.errors++;
            return;
        }

        js_conn_set_request(c, cfg->requests[next_idx].data,
                             cfg->requests[next_idx].len);
        js_epoll_add(engine, &c->socket, EPOLLIN | EPOLLOUT | EPOLLET);
    } else {
        /* Still in progress, update epoll interest */
        uint32_t mask = EPOLLET;
        if (c->state == CONN_CONNECTING || c->state == CONN_WRITING ||
            c->state == CONN_TLS_HANDSHAKE)
            mask |= EPOLLOUT | EPOLLIN;
        else
            mask |= EPOLLIN;
        js_epoll_mod(engine, &c->socket, mask);
    }
}

static void worker_on_read(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    js_http_response_t *r = c->socket.data;
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

    worker_conn_process(c);
}

static void worker_on_write(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    js_conn_process_write(c);
    worker_conn_process(c);
}

static void worker_on_error(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    c->state = CONN_ERROR;
    worker_conn_process(c);
}

/* ── C-path worker: string/object/array modes ─────────────────────────── */

static void worker_c_path(js_worker_t *w) {
    js_config_t *cfg = w->config;

    js_engine_t *engine = js_engine_create();
    if (engine == NULL) return;
    w->engine = engine;

    /* Duration timer */
    js_timer_t duration_timer = {0};

    if (cfg->duration_sec > 0) {
        duration_timer.handler = worker_duration_handler;
        duration_timer.data = &w->stop;
        js_timer_add(&engine->timers, &duration_timer,
                     (js_msec_t)(cfg->duration_sec * 1000));
    }

    /* Create connections */
    js_conn_t **conns = calloc((size_t)w->conn_count, sizeof(js_conn_t *));
    js_http_response_t *responses = calloc((size_t)w->conn_count,
                                            sizeof(js_http_response_t));
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

        js_http_response_init(&responses[i]);
        conns[i]->socket.data  = &responses[i];
        conns[i]->socket.read  = worker_on_read;
        conns[i]->socket.write = worker_on_write;
        conns[i]->socket.error = worker_on_error;
        conns[i]->udata = w;

        /* Assign request (round-robin for array mode) */
        int req_idx = i % cfg->request_count;
        conns[i]->req_index = req_idx;
        js_conn_set_request(conns[i], cfg->requests[req_idx].data,
                             cfg->requests[req_idx].len);

        js_epoll_add(engine, &conns[i]->socket, EPOLLIN | EPOLLOUT | EPOLLET);
        active++;
    }

    /* Event loop */
    while (!atomic_load(&w->stop) && active > 0) {
        js_msec_t timer_timeout = js_timer_find(&engine->timers);
        int timeout;

        if (timer_timeout == (js_msec_t) -1) {
            timeout = 100;
        } else {
            timeout = (int) timer_timeout;
            if (timeout > 100) timeout = 100;
        }

        if (js_epoll_poll(engine, timeout) < 0) break;

        engine->timers.now = (js_msec_t) (js_now_ns() / 1000000);
        js_timer_expire(&engine->timers, engine->timers.now);
    }

    /* Cleanup */
    for (int i = 0; i < w->conn_count; i++) {
        js_http_response_free(&responses[i]);
        if (conns[i]) js_conn_free(conns[i]);
    }
    free(responses);
    free(conns);
    js_engine_destroy(engine);
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
