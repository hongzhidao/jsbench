#include "js_main.h"

/* TODO: use config->target as base URL for relative fetch() paths */

/* ── Headers class ────────────────────────────────────────────────────── */

typedef struct {
    js_header_t *entries;
    int           count;
    int           cap;
} js_headers_t;

static JSClassID js_headers_class_id;
static JSClassID js_response_class_id;

static void js_headers_finalizer(JSRuntime *rt, JSValue val) {
    js_headers_t *h = JS_GetOpaque(val, js_headers_class_id);
    if (h) {
        free(h->entries);
        js_free_rt(rt, h);
    }
}

static JSClassDef js_headers_class = {
    "Headers",
    .finalizer = js_headers_finalizer,
};

static js_headers_t *js_headers_create(JSContext *ctx) {
    js_headers_t *h = js_mallocz(ctx, sizeof(js_headers_t));
    h->cap = 16;
    h->entries = malloc(sizeof(js_header_t) * (size_t)h->cap);
    h->count = 0;
    return h;
}

static void js_headers_set(js_headers_t *h, const char *name, const char *value) {
    /* Check for existing (case-insensitive) */
    for (int i = 0; i < h->count; i++) {
        if (strcasecmp(h->entries[i].name, name) == 0) {
            snprintf(h->entries[i].value, sizeof(h->entries[i].value), "%s", value);
            return;
        }
    }
    /* Add new */
    if (h->count >= h->cap) {
        h->cap *= 2;
        h->entries = realloc(h->entries, sizeof(js_header_t) * (size_t)h->cap);
    }
    snprintf(h->entries[h->count].name, sizeof(h->entries[h->count].name), "%s", name);
    snprintf(h->entries[h->count].value, sizeof(h->entries[h->count].value), "%s", value);
    h->count++;
}

static JSValue js_headers_get(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    js_headers_t *h = JS_GetOpaque(this_val, js_headers_class_id);
    if (!h || argc < 1) return JS_NULL;

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;

    for (int i = 0; i < h->count; i++) {
        if (strcasecmp(h->entries[i].name, name) == 0) {
            JSValue ret = JS_NewString(ctx, h->entries[i].value);
            JS_FreeCString(ctx, name);
            return ret;
        }
    }
    JS_FreeCString(ctx, name);
    return JS_NULL;
}

static JSValue js_headers_has(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    js_headers_t *h = JS_GetOpaque(this_val, js_headers_class_id);
    if (!h || argc < 1) return JS_FALSE;

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;

    for (int i = 0; i < h->count; i++) {
        if (strcasecmp(h->entries[i].name, name) == 0) {
            JS_FreeCString(ctx, name);
            return JS_TRUE;
        }
    }
    JS_FreeCString(ctx, name);
    return JS_FALSE;
}

static JSValue js_headers_set_fn(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    js_headers_t *h = JS_GetOpaque(this_val, js_headers_class_id);
    if (!h || argc < 2) return JS_UNDEFINED;

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (name && value) js_headers_set(h, name, value);
    if (name) JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue js_headers_delete(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    js_headers_t *h = JS_GetOpaque(this_val, js_headers_class_id);
    if (!h || argc < 1) return JS_UNDEFINED;

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;

    for (int i = 0; i < h->count; i++) {
        if (strcasecmp(h->entries[i].name, name) == 0) {
            /* Shift remaining entries */
            memmove(&h->entries[i], &h->entries[i+1],
                    sizeof(js_header_t) * (size_t)(h->count - i - 1));
            h->count--;
            break;
        }
    }
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_headers_forEach(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    js_headers_t *h = JS_GetOpaque(this_val, js_headers_class_id);
    if (!h || argc < 1) return JS_UNDEFINED;

    JSValue cb = argv[0];
    if (!JS_IsFunction(ctx, cb)) return JS_UNDEFINED;

    for (int i = 0; i < h->count; i++) {
        JSValue args[2];
        args[0] = JS_NewString(ctx, h->entries[i].value);
        args[1] = JS_NewString(ctx, h->entries[i].name);
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(ret)) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, ret);
    }
    return JS_UNDEFINED;
}

static JSValue js_headers_new_obj(JSContext *ctx, js_headers_t *h) {
    JSValue obj = JS_NewObjectClass(ctx, (int)js_headers_class_id);
    JS_SetOpaque(obj, h);
    return obj;
}

/* ── Response class ───────────────────────────────────────────────────── */

typedef struct {
    int          status;
    char        *status_text;
    char        *body;
    size_t       body_len;
    JSValue      headers_obj;  /* JS Headers object */
    bool         body_used;
} js_response_t;

static void js_response_finalizer(JSRuntime *rt, JSValue val) {
    js_response_t *r = JS_GetOpaque(val, js_response_class_id);
    if (r) {
        free(r->status_text);
        free(r->body);
        JS_FreeValueRT(rt, r->headers_obj);
        js_free_rt(rt, r);
    }
}

static void js_response_gc_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func) {
    js_response_t *r = JS_GetOpaque(val, js_response_class_id);
    if (r) {
        JS_MarkValue(rt, r->headers_obj, mark_func);
    }
}

static JSClassDef js_response_class = {
    "Response",
    .finalizer = js_response_finalizer,
    .gc_mark = js_response_gc_mark,
};

static JSValue js_response_get_status(JSContext *ctx, JSValueConst this_val) {
    js_response_t *r = JS_GetOpaque(this_val, js_response_class_id);
    if (!r) return JS_UNDEFINED;
    return JS_NewInt32(ctx, r->status);
}

static JSValue js_response_get_status_text(JSContext *ctx, JSValueConst this_val) {
    js_response_t *r = JS_GetOpaque(this_val, js_response_class_id);
    if (!r) return JS_UNDEFINED;
    return JS_NewString(ctx, r->status_text ? r->status_text : "");
}

static JSValue js_response_get_headers(JSContext *ctx, JSValueConst this_val) {
    js_response_t *r = JS_GetOpaque(this_val, js_response_class_id);
    if (!r) return JS_UNDEFINED;
    return JS_DupValue(ctx, r->headers_obj);
}

static JSValue js_response_get_ok(JSContext *ctx, JSValueConst this_val) {
    js_response_t *r = JS_GetOpaque(this_val, js_response_class_id);
    if (!r) return JS_FALSE;
    return JS_NewBool(ctx, r->status >= 200 && r->status < 300);
}

/* text() returns a resolved promise with the body string */
static JSValue js_response_text(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    js_response_t *r = JS_GetOpaque(this_val, js_response_class_id);
    if (!r) return JS_EXCEPTION;

    JSValue str = JS_NewStringLen(ctx, r->body ? r->body : "", r->body_len);

    /* Wrap in resolved promise */
    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
    JSValue ret = JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 1, &str);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, resolve_funcs[0]);
    JS_FreeValue(ctx, resolve_funcs[1]);
    return promise;
}

/* json() returns a resolved promise with parsed JSON */
static JSValue js_response_json(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    js_response_t *r = JS_GetOpaque(this_val, js_response_class_id);
    if (!r) return JS_EXCEPTION;

    JSValue parsed = JS_ParseJSON(ctx, r->body ? r->body : "{}", r->body_len, "<json>");
    if (JS_IsException(parsed)) return parsed;

    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
    JSValue ret = JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 1, &parsed);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, parsed);
    JS_FreeValue(ctx, resolve_funcs[0]);
    JS_FreeValue(ctx, resolve_funcs[1]);
    return promise;
}

JSValue js_response_new(JSContext *ctx, int status, const char *status_text,
                                const char *body, size_t body_len,
                                const js_http_response_t *parsed) {
    js_response_t *r = js_mallocz(ctx, sizeof(js_response_t));
    r->status = status;
    r->status_text = strdup(status_text ? status_text : "");
    if (body && body_len > 0) {
        r->body = malloc(body_len + 1);
        memcpy(r->body, body, body_len);
        r->body[body_len] = '\0';
        r->body_len = body_len;
    } else {
        r->body = strdup("");
        r->body_len = 0;
    }

    /* Create Headers object */
    js_headers_t *h = js_headers_create(ctx);
    if (parsed) {
        for (int i = 0; i < parsed->header_count; i++) {
            js_headers_set(h, parsed->headers[i].name, parsed->headers[i].value);
        }
    }
    r->headers_obj = js_headers_new_obj(ctx, h);

    JSValue obj = JS_NewObjectClass(ctx, (int)js_response_class_id);
    JS_SetOpaque(obj, r);
    return obj;
}

/* ── fetch() implementation ───────────────────────────────────────────── */

/*
 * Synchronous (blocking) fetch for use in the QuickJS event loop.
 * Returns a Promise that resolves with a Response.
 *
 * The actual I/O is blocking within this call, but we return
 * a Promise to maintain API compatibility with the Web Fetch API.
 */
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
            free(raw.data);
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

    js_conn_set_request(conn, raw.data, raw.len);

    /* Register with event loop — return a pending promise */
    js_loop_t *loop = JS_GetContextOpaque(ctx);
    if (!loop) {
        js_conn_free(conn);
        free(raw.data);
        if (ssl_ctx) SSL_CTX_free(ssl_ctx);
        JS_FreeCString(ctx, url_str);
        if (method_str) JS_FreeCString(ctx, method_str);
        if (body_str) JS_FreeCString(ctx, body_str);
        return JS_ThrowInternalError(ctx, "No event loop");
    }

    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);

    js_loop_add(loop, conn, raw.data, ssl_ctx, ctx,
                resolve_funcs[0], resolve_funcs[1]);

    JS_FreeCString(ctx, url_str);
    if (method_str) JS_FreeCString(ctx, method_str);
    if (body_str) JS_FreeCString(ctx, body_str);

    return promise;
}

/* ── Module initialization ────────────────────────────────────────────── */

static const JSCFunctionListEntry js_headers_proto_funcs[] = {
    JS_CFUNC_DEF("get", 1, js_headers_get),
    JS_CFUNC_DEF("has", 1, js_headers_has),
    JS_CFUNC_DEF("set", 2, js_headers_set_fn),
    JS_CFUNC_DEF("delete", 1, js_headers_delete),
    JS_CFUNC_DEF("forEach", 1, js_headers_forEach),
};

static const JSCFunctionListEntry js_response_proto_funcs[] = {
    JS_CGETSET_DEF("status", js_response_get_status, NULL),
    JS_CGETSET_DEF("statusText", js_response_get_status_text, NULL),
    JS_CGETSET_DEF("headers", js_response_get_headers, NULL),
    JS_CGETSET_DEF("ok", js_response_get_ok, NULL),
    JS_CFUNC_DEF("text", 0, js_response_text),
    JS_CFUNC_DEF("json", 0, js_response_json),
};

void js_fetch_init(JSContext *ctx) {
    /* Register Headers class */
    JS_NewClassID(&js_headers_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_headers_class_id, &js_headers_class);

    JSValue headers_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, headers_proto, js_headers_proto_funcs,
                               sizeof(js_headers_proto_funcs) / sizeof(js_headers_proto_funcs[0]));
    JS_SetClassProto(ctx, js_headers_class_id, headers_proto);

    /* Register Response class */
    JS_NewClassID(&js_response_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_response_class_id, &js_response_class);

    JSValue response_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, response_proto, js_response_proto_funcs,
                               sizeof(js_response_proto_funcs) / sizeof(js_response_proto_funcs[0]));
    JS_SetClassProto(ctx, js_response_class_id, response_proto);

    /* Register global fetch() */
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_FreeValue(ctx, global);
}
