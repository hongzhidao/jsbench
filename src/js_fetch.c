#include "js_main.h"

/* TODO: use config->target as base URL for relative fetch() paths */

/* ── Fetch lifecycle ─────────────────────────────────────────────────── */

void js_fetch_destroy(js_fetch_t *f) {
    js_pending_t *p = &f->pending;
    JSContext *ctx = p->ctx;

    js_epoll_del(js_thread()->engine, &f->conn->socket);
    js_timer_delete(&js_thread()->engine->timers, &f->timer);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_http_response_free(&f->response);
    js_conn_free(f->conn);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    list_del(&p->link);
    free(f);
}

static void js_fetch_complete(js_fetch_t *f) {
    js_pending_t *p = &f->pending;
    js_http_response_t *r = &f->response;
    JSContext *ctx = p->ctx;

    JSValue response = js_response_new(ctx,
        r->status_code, r->status_text,
        r->body, r->body_len, r);

    JSValue ret = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, &response);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, response);

    js_fetch_destroy(f);
}

static void js_fetch_fail(js_fetch_t *f, const char *message) {
    js_pending_t *p = &f->pending;
    JSContext *ctx = p->ctx;

    JSValue err = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, err, "message",
                              JS_NewString(ctx, message),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JSValue ret = JS_Call(ctx, p->reject, JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, err);

    js_fetch_destroy(f);
}

/* ── Connection event handlers ───────────────────────────────────────── */

static void js_fetch_process(js_conn_t *c) {
    js_fetch_t *f = c->socket.data;

    if (c->state == CONN_DONE) {
        js_fetch_complete(f);
    } else if (c->state == CONN_ERROR) {
        js_fetch_fail(f, "Connection error");
    } else {
        uint32_t mask = EPOLLET;
        if (c->state == CONN_CONNECTING || c->state == CONN_WRITING ||
            c->state == CONN_TLS_HANDSHAKE)
            mask |= EPOLLOUT | EPOLLIN;
        else
            mask |= EPOLLIN;
        js_epoll_mod(js_thread()->engine, &c->socket, mask);
    }
}

static void js_fetch_on_read(js_event_t *ev) {
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

    js_fetch_process(c);
}

static void js_fetch_on_write(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    js_conn_process_write(c);
    js_fetch_process(c);
}

static void js_fetch_on_error(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    c->state = CONN_ERROR;
    js_fetch_process(c);
}

/* ── Fetch timeout handler ────────────────────────────────────────────── */

static void js_fetch_timeout_handler(js_timer_t *timer, void *data) {
    js_pending_t *p = data;
    js_fetch_fail(js_fetch_from_pending(p), "Request timeout");
}

/* ── fetch() implementation ───────────────────────────────────────────── */

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;

    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_EXCEPTION;

    /* Parse options */
    const char *method = "GET";
    const char *body = NULL;
    size_t body_len = 0;
    char extra_headers[4096] = "";
    const char *method_str = NULL;
    const char *body_str = NULL;

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue opt = argv[1];

        JSValue v_method = JS_GetPropertyStr(ctx, opt, "method");
        if (JS_IsString(v_method)) {
            method_str = JS_ToCString(ctx, v_method);
            method = method_str;
        }
        JS_FreeValue(ctx, v_method);

        JSValue v_body = JS_GetPropertyStr(ctx, opt, "body");
        if (JS_IsString(v_body)) {
            body_str = JS_ToCString(ctx, v_body);
            body = body_str;
            body_len = strlen(body);
        }
        JS_FreeValue(ctx, v_body);

        JSValue v_headers = JS_GetPropertyStr(ctx, opt, "headers");
        if (JS_IsObject(v_headers)) {
            JSPropertyEnum *tab;
            uint32_t len;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, v_headers,
                                        JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) == 0) {
                int off = 0;
                for (uint32_t i = 0; i < len; i++) {
                    JSValue key = JS_AtomToString(ctx, tab[i].atom);
                    JSValue val = JS_GetProperty(ctx, v_headers, tab[i].atom);
                    const char *k = JS_ToCString(ctx, key);
                    const char *v = JS_ToCString(ctx, val);
                    if (k && v)
                        off += snprintf(extra_headers + off,
                                       sizeof(extra_headers) - (size_t)off,
                                       "%s: %s\r\n", k, v);
                    if (k) JS_FreeCString(ctx, k);
                    if (v) JS_FreeCString(ctx, v);
                    JS_FreeValue(ctx, key);
                    JS_FreeValue(ctx, val);
                    JS_FreeAtom(ctx, tab[i].atom);
                }
                js_free(ctx, tab);
            }
        }
        JS_FreeValue(ctx, v_headers);
    }

    /* Parse URL */
    js_url_t url;
    if (js_parse_url(url_str, &url) != 0) {
        JS_FreeCString(ctx, url_str);
        if (method_str) JS_FreeCString(ctx, method_str);
        if (body_str) JS_FreeCString(ctx, body_str);
        return JS_ThrowTypeError(ctx, "Invalid URL");
    }

    /* Resolve DNS */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int gai_err = getaddrinfo(url.host, url.port_str, &hints, &res);
    if (gai_err != 0 || !res) {
        JS_FreeCString(ctx, url_str);
        if (method_str) JS_FreeCString(ctx, method_str);
        if (body_str) JS_FreeCString(ctx, body_str);
        return JS_ThrowTypeError(ctx, "DNS resolution failed: %s", gai_strerror(gai_err));
    }

    /* Build HTTP request */
    js_request_t req = {
        .url = url,
        .method = (char *)method,
        .headers = extra_headers[0] ? extra_headers : NULL,
        .body = (char *)body,
        .body_len = body_len,
    };
    js_buf_t raw = {0};
    if (js_request_serialize(&req, NULL, &raw) != 0) {
        freeaddrinfo(res);
        JS_FreeCString(ctx, url_str);
        if (method_str) JS_FreeCString(ctx, method_str);
        if (body_str) JS_FreeCString(ctx, body_str);
        return JS_ThrowInternalError(ctx, "Failed to build HTTP request");
    }

    /* Create TLS context if needed */
    SSL_CTX *ssl_ctx = NULL;
    if (url.is_tls) {
        ssl_ctx = js_tls_ctx_create();
        if (!ssl_ctx) {
            js_buf_free(&raw);
            freeaddrinfo(res);
            JS_FreeCString(ctx, url_str);
            if (method_str) JS_FreeCString(ctx, method_str);
            if (body_str) JS_FreeCString(ctx, body_str);
            return JS_ThrowInternalError(ctx, "TLS init failed");
        }
    }

    /* Create connection */
    js_conn_t *conn = js_conn_create(res->ai_addr, res->ai_addrlen, ssl_ctx, url.host);
    freeaddrinfo(res);
    if (!conn) {
        free(raw.data);
        if (ssl_ctx) SSL_CTX_free(ssl_ctx);
        JS_FreeCString(ctx, url_str);
        if (method_str) JS_FreeCString(ctx, method_str);
        if (body_str) JS_FreeCString(ctx, body_str);
        return JS_ThrowInternalError(ctx, "Connection failed");
    }

    js_conn_set_output(conn, raw.data, raw.len);
    js_buf_free(&raw);

    /* Register with event loop — return a pending promise */
    js_loop_t *loop = JS_GetContextOpaque(ctx);
    if (!loop) {
        js_conn_free(conn);
        if (ssl_ctx) SSL_CTX_free(ssl_ctx);
        JS_FreeCString(ctx, url_str);
        if (method_str) JS_FreeCString(ctx, method_str);
        if (body_str) JS_FreeCString(ctx, body_str);
        return JS_ThrowInternalError(ctx, "No event loop");
    }

    js_fetch_t *f = calloc(1, sizeof(js_fetch_t));
    if (!f) {
        js_conn_free(conn);
        if (ssl_ctx) SSL_CTX_free(ssl_ctx);
        JS_FreeCString(ctx, url_str);
        if (method_str) JS_FreeCString(ctx, method_str);
        if (body_str) JS_FreeCString(ctx, body_str);
        return JS_ThrowInternalError(ctx, "Out of memory");
    }

    js_http_response_init(&f->response);
    f->conn = conn;
    conn->socket.data = f;
    conn->socket.read  = js_fetch_on_read;
    conn->socket.write = js_fetch_on_write;
    conn->socket.error = js_fetch_on_error;

    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);

    js_pending_t *p = &f->pending;
    p->ssl_ctx = ssl_ctx;
    p->ctx = ctx;
    p->resolve = resolve_funcs[0];
    p->reject = resolve_funcs[1];

    f->timer.handler = js_fetch_timeout_handler;
    f->timer.data = p;

    js_engine_t *engine = js_thread()->engine;
    engine->timers.now = (js_msec_t)(js_now_ns() / 1000000);
    js_timer_add(&engine->timers, &f->timer, 30 * 1000);

    js_loop_add(loop, p);

    JS_FreeCString(ctx, url_str);
    if (method_str) JS_FreeCString(ctx, method_str);
    if (body_str) JS_FreeCString(ctx, body_str);

    return promise;
}

/* ── Module initialization ────────────────────────────────────────────── */

void js_fetch_init(JSContext *ctx) {
    js_headers_init(ctx);
    js_response_init(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_FreeValue(ctx, global);
}
