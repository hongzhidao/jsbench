#include "jsb.h"

/* ── console.log binding ──────────────────────────────────────────────── */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) putchar(' ');
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            fputs(str, stdout);
            JS_FreeCString(ctx, str);
        }
    }
    putchar('\n');
    fflush(stdout);
    return JS_UNDEFINED;
}

void jsb_vm_setup_console(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_log, "error", 1));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, js_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/* ── Unhandled promise rejection tracker ──────────────────────────────── */

int jsb_had_unhandled_rejection = 0;

static void jsb_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                          JSValueConst reason,
                                          JS_BOOL is_handled, void *opaque) {
    (void)promise;
    (void)opaque;
    if (is_handled) return;
    /* Only print the first unhandled rejection to avoid duplicates
     * from promise chain propagation (fetch reject -> await reject). */
    if (!jsb_had_unhandled_rejection) {
        const char *str = JS_ToCString(ctx, reason);
        if (str) {
            fprintf(stderr, "Error: %s\n", str);
            JS_FreeCString(ctx, str);
        }
    }
    jsb_had_unhandled_rejection = 1;
}

/* ── Runtime / Context creation ───────────────────────────────────────── */

JSRuntime *jsb_vm_rt_create(void) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return NULL;
    JS_SetMaxStackSize(rt, 1024 * 1024);  /* 1MB stack */
    JS_SetMemoryLimit(rt, 128 * 1024 * 1024);  /* 128MB memory */
    JS_SetHostPromiseRejectionTracker(rt, jsb_promise_rejection_tracker, NULL);
    return rt;
}

JSContext *jsb_vm_ctx_create(JSRuntime *rt) {
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return NULL;

    /* Enable ES module support */
    JS_SetModuleLoaderFunc(rt, NULL, NULL, NULL);

    /* Set up console and fetch */
    jsb_vm_setup_console(ctx);
    jsb_fetch_init(ctx);

    return ctx;
}

/* ── Module evaluation ────────────────────────────────────────────────── */

/*
 * Evaluate an ES module and extract 'default' and 'bench' exports.
 * We wrap the user script in a module evaluation context.
 */
int jsb_vm_eval_module(JSContext *ctx, const char *filename,
                       const char *source, JSValue *default_export,
                       JSValue *bench_export) {
    *default_export = JS_UNDEFINED;
    *bench_export = JS_UNDEFINED;

    /* Compile as module */
    JSValue val = JS_Eval(ctx, source, strlen(source), filename,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        if (str) {
            fprintf(stderr, "Compile error: %s\n", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
        return -1;
    }

    /* Get the JSModuleDef* from the compiled module value */
    JSModuleDef *m = JS_VALUE_GET_PTR(val);

    /* Evaluate the module (consumes val) */
    JSValue result = JS_EvalFunction(ctx, val);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        if (str) {
            fprintf(stderr, "Runtime error: %s\n", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
        return -1;
    }
    JS_FreeValue(ctx, result);

    /* Execute pending jobs (promises, top-level await, etc.) */
    JSContext *pctx;
    int job_ret;
    for (;;) {
        job_ret = JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx);
        if (job_ret <= 0) break;
    }
    if (job_ret < 0) {
        /* Consume the exception so it doesn't linger.
         * The promise rejection tracker already printed the error. */
        JSValue exc = JS_GetException(pctx);
        JS_FreeValue(pctx, exc);
    }

    /* Get module namespace and extract exports */
    JSValue ns = JS_GetModuleNamespace(ctx, m);
    if (JS_IsObject(ns)) {
        *default_export = JS_GetPropertyStr(ctx, ns, "default");
        *bench_export = JS_GetPropertyStr(ctx, ns, "bench");
    }
    JS_FreeValue(ctx, ns);

    return 0;
}

/* ── Detect mode from default export ──────────────────────────────────── */

jsb_mode_t jsb_vm_detect_mode(JSContext *ctx, JSValue default_export) {
    if (JS_IsUndefined(default_export) || JS_IsNull(default_export) ||
        JS_IsUninitialized(default_export))
        return MODE_CLI;

    if (JS_IsString(default_export))
        return MODE_BENCH_STRING;

    if (JS_IsFunction(ctx, default_export))
        return MODE_BENCH_ASYNC;

    if (JS_IsArray(ctx, default_export))
        return MODE_BENCH_ARRAY;

    if (JS_IsObject(default_export))
        return MODE_BENCH_OBJECT;

    return MODE_CLI;
}

/* ── Extract bench config ─────────────────────────────────────────────── */

int jsb_vm_extract_config(JSContext *ctx, JSValue bench_export,
                          jsb_config_t *config) {
    if (JS_IsUndefined(bench_export) || !JS_IsObject(bench_export))
        return 0;

    JSValue v;

    v = JS_GetPropertyStr(ctx, bench_export, "connections");
    if (JS_IsNumber(v)) {
        int32_t n;
        JS_ToInt32(ctx, &n, v);
        if (n > 0) config->connections = n;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, bench_export, "threads");
    if (JS_IsNumber(v)) {
        int32_t n;
        JS_ToInt32(ctx, &n, v);
        if (n > 0) config->threads = n;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, bench_export, "duration");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            config->duration_sec = jsb_parse_duration(s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, bench_export, "target");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            config->target = strdup(s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, bench_export, "host");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            config->host = strdup(s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);

    return 0;
}

/* ── Extract request descriptors ──────────────────────────────────────── */

static int extract_single_request(JSContext *ctx, JSValue val,
                                  jsb_config_t *config, const char *target_override) {
    jsb_request_desc_t desc = {0};
    jsb_url_t url;

    if (JS_IsString(val)) {
        const char *s = JS_ToCString(ctx, val);
        if (!s) return -1;

        /* If target override, combine target base with path */
        if (target_override) {
            jsb_url_t turl;
            if (jsb_parse_url(target_override, &turl) != 0) {
                JS_FreeCString(ctx, s);
                return -1;
            }
            /* If val is a path (starts with /), combine with target */
            if (s[0] == '/') {
                char full_url[JSB_MAX_URL_LEN];
                snprintf(full_url, sizeof(full_url), "%s://%s:%d%s",
                         turl.scheme, turl.host, turl.port, s);
                if (jsb_parse_url(full_url, &url) != 0) {
                    JS_FreeCString(ctx, s);
                    return -1;
                }
            } else {
                if (jsb_parse_url(s, &url) != 0) {
                    JS_FreeCString(ctx, s);
                    return -1;
                }
                /* Override host/port from target */
                strcpy(url.host, turl.host);
                url.port = turl.port;
                strcpy(url.port_str, turl.port_str);
                url.is_tls = turl.is_tls;
                strcpy(url.scheme, turl.scheme);
            }
        } else {
            if (jsb_parse_url(s, &url) != 0) {
                JS_FreeCString(ctx, s);
                return -1;
            }
        }
        JS_FreeCString(ctx, s);
        desc.method = "GET";
    } else if (JS_IsObject(val)) {
        JSValue v_url = JS_GetPropertyStr(ctx, val, "url");
        JSValue v_method = JS_GetPropertyStr(ctx, val, "method");
        JSValue v_body = JS_GetPropertyStr(ctx, val, "body");
        JSValue v_headers = JS_GetPropertyStr(ctx, val, "headers");

        const char *url_s = JS_ToCString(ctx, v_url);
        if (!url_s) {
            JS_FreeValue(ctx, v_url);
            JS_FreeValue(ctx, v_method);
            JS_FreeValue(ctx, v_body);
            JS_FreeValue(ctx, v_headers);
            return -1;
        }

        /* Handle path-only URLs with target override */
        if (target_override && url_s[0] == '/') {
            jsb_url_t turl;
            if (jsb_parse_url(target_override, &turl) == 0) {
                char full_url[JSB_MAX_URL_LEN];
                snprintf(full_url, sizeof(full_url), "%s://%s:%d%s",
                         turl.scheme, turl.host, turl.port, url_s);
                jsb_parse_url(full_url, &url);
            } else {
                jsb_parse_url(url_s, &url);
            }
        } else if (target_override) {
            jsb_parse_url(url_s, &url);
            jsb_url_t turl;
            if (jsb_parse_url(target_override, &turl) == 0) {
                strcpy(url.host, turl.host);
                url.port = turl.port;
                strcpy(url.port_str, turl.port_str);
                url.is_tls = turl.is_tls;
                strcpy(url.scheme, turl.scheme);
            }
        } else {
            jsb_parse_url(url_s, &url);
        }
        JS_FreeCString(ctx, url_s);

        if (JS_IsString(v_method)) {
            const char *m = JS_ToCString(ctx, v_method);
            desc.method = strdup(m);
            JS_FreeCString(ctx, m);
        } else {
            desc.method = "GET";
        }

        if (JS_IsString(v_body)) {
            const char *b = JS_ToCString(ctx, v_body);
            desc.body = strdup(b);
            desc.body_len = strlen(b);
            JS_FreeCString(ctx, b);
        }

        /* Process headers object into "Key: Value\r\n" format */
        if (JS_IsObject(v_headers)) {
            char hdr_buf[4096] = "";
            int off = 0;
            JSPropertyEnum *tab;
            uint32_t len;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, v_headers,
                                        JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) == 0) {
                for (uint32_t i = 0; i < len; i++) {
                    JSValue key = JS_AtomToString(ctx, tab[i].atom);
                    JSValue hval = JS_GetProperty(ctx, v_headers, tab[i].atom);
                    const char *k = JS_ToCString(ctx, key);
                    const char *hv = JS_ToCString(ctx, hval);
                    if (k && hv)
                        off += snprintf(hdr_buf + off, sizeof(hdr_buf) - (size_t)off,
                                       "%s: %s\r\n", k, hv);
                    if (k) JS_FreeCString(ctx, k);
                    if (hv) JS_FreeCString(ctx, hv);
                    JS_FreeValue(ctx, key);
                    JS_FreeValue(ctx, hval);
                    JS_FreeAtom(ctx, tab[i].atom);
                }
                js_free(ctx, tab);
            }
            if (off > 0) desc.headers = strdup(hdr_buf);
        }

        JS_FreeValue(ctx, v_url);
        JS_FreeValue(ctx, v_method);
        JS_FreeValue(ctx, v_body);
        JS_FreeValue(ctx, v_headers);
    } else {
        return -1;
    }

    /* Serialize the request */
    config->requests = realloc(config->requests,
                               sizeof(jsb_raw_request_t) * (size_t)(config->request_count + 1));
    jsb_raw_request_t *raw = &config->requests[config->request_count];

    if (jsb_serialize_request(&desc, &url, config->host, raw) != 0) {
        /* cleanup */
        if (desc.method && desc.method != (char*)"GET" && desc.method != (char*)"POST")
            free(desc.method);
        free(desc.body);
        free(desc.headers);
        return -1;
    }

    config->request_count++;

    /* Set TLS flag from first request */
    if (config->request_count == 1)
        config->use_tls = url.is_tls;

    /* Cleanup heap-allocated desc fields */
    if (desc.body) free(desc.body);
    if (desc.headers) free(desc.headers);
    /* method: only free if we strdup'd it (i.e., it's not a literal) */
    if (desc.method && strcmp(desc.method, "GET") != 0) {
        /* Heuristic: only free if we allocated it above */
        /* This is a simplification -- in production we'd track ownership */
    }

    return 0;
}

int jsb_vm_extract_requests(JSContext *ctx, JSValue default_export,
                            jsb_config_t *config) {
    config->requests = NULL;
    config->request_count = 0;

    const char *target = config->target;

    if (JS_IsString(default_export) || JS_IsObject(default_export)) {
        if (JS_IsArray(ctx, default_export)) {
            /* Array of requests */
            JSValue len_val = JS_GetPropertyStr(ctx, default_export, "length");
            int32_t len;
            JS_ToInt32(ctx, &len, len_val);
            JS_FreeValue(ctx, len_val);

            for (int32_t i = 0; i < len; i++) {
                JSValue item = JS_GetPropertyUint32(ctx, default_export, (uint32_t)i);
                if (extract_single_request(ctx, item, config, target) != 0) {
                    JS_FreeValue(ctx, item);
                    return -1;
                }
                JS_FreeValue(ctx, item);
            }
        } else {
            return extract_single_request(ctx, default_export, config, target);
        }
    }

    return 0;
}
