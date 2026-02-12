#include "js_main.h"

typedef struct {
    int          status;
    char        *status_text;
    char        *body;
    size_t       body_len;
    JSValue      headers_obj;  /* JS Headers object */
    bool         body_used;
} js_response_t;

static JSClassID js_response_class_id;

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

/* ── Public API ──────────────────────────────────────────────────────── */

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

    r->headers_obj = js_headers_from_http(ctx, parsed);

    JSValue obj = JS_NewObjectClass(ctx, (int)js_response_class_id);
    JS_SetOpaque(obj, r);
    return obj;
}

static const JSCFunctionListEntry js_response_proto_funcs[] = {
    JS_CGETSET_DEF("status", js_response_get_status, NULL),
    JS_CGETSET_DEF("statusText", js_response_get_status_text, NULL),
    JS_CGETSET_DEF("headers", js_response_get_headers, NULL),
    JS_CGETSET_DEF("ok", js_response_get_ok, NULL),
    JS_CFUNC_DEF("text", 0, js_response_text),
    JS_CFUNC_DEF("json", 0, js_response_json),
};

void js_response_init(JSContext *ctx) {
    JS_NewClassID(&js_response_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_response_class_id, &js_response_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_response_proto_funcs,
                               countof(js_response_proto_funcs));
    JS_SetClassProto(ctx, js_response_class_id, proto);
}
