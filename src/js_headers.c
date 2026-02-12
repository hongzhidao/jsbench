#include "js_main.h"

typedef struct {
    js_header_t *entries;
    int           count;
    int           cap;
} js_headers_t;

static JSClassID js_headers_class_id;

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

/* ── Public API ──────────────────────────────────────────────────────── */

JSValue js_headers_from_http(JSContext *ctx, const js_http_response_t *parsed) {
    js_headers_t *h = js_headers_create(ctx);

    if (parsed) {
        for (int i = 0; i < parsed->header_count; i++) {
            js_headers_set(h, parsed->headers[i].name, parsed->headers[i].value);
        }
    }

    return js_headers_new_obj(ctx, h);
}

static const JSCFunctionListEntry js_headers_proto_funcs[] = {
    JS_CFUNC_DEF("get", 1, js_headers_get),
    JS_CFUNC_DEF("has", 1, js_headers_has),
    JS_CFUNC_DEF("set", 2, js_headers_set_fn),
    JS_CFUNC_DEF("delete", 1, js_headers_delete),
    JS_CFUNC_DEF("forEach", 1, js_headers_forEach),
};

void js_headers_init(JSContext *ctx) {
    JS_NewClassID(&js_headers_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_headers_class_id, &js_headers_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_headers_proto_funcs,
                               countof(js_headers_proto_funcs));
    JS_SetClassProto(ctx, js_headers_class_id, proto);
}
