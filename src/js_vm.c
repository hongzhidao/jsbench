#include "js_main.h"

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

static void js_vm_setup_console(JSContext *ctx) {
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

int js_had_unhandled_rejection = 0;

static void js_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                          JSValueConst reason,
                                          JS_BOOL is_handled, void *opaque) {
    (void)promise;
    (void)opaque;
    if (is_handled) return;
    /* Only print the first unhandled rejection to avoid duplicates
     * from promise chain propagation (fetch reject -> await reject). */
    if (!js_had_unhandled_rejection) {
        const char *str = JS_ToCString(ctx, reason);
        if (str) {
            fprintf(stderr, "Error: %s\n", str);
            JS_FreeCString(ctx, str);
        }
    }
    js_had_unhandled_rejection = 1;
}

/* ── Create / free ────────────────────────────────────────────────────── */

JSContext *js_vm_create(void) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return NULL;

    JS_SetMaxStackSize(rt, 1024 * 1024);  /* 1MB stack */
    JS_SetMemoryLimit(rt, 128 * 1024 * 1024);  /* 128MB memory */
    JS_SetHostPromiseRejectionTracker(rt, js_promise_rejection_tracker, NULL);

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return NULL;
    }

    /* Enable ES module support */
    JS_SetModuleLoaderFunc(rt, NULL, NULL, NULL);

    /* Set up console and fetch */
    js_vm_setup_console(ctx);
    js_fetch_init(ctx);

    return ctx;
}

void js_vm_free(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* ── Module evaluation ────────────────────────────────────────────────── */

/*
 * Evaluate an ES module and extract 'default' and 'bench' exports.
 * We wrap the user script in a module evaluation context.
 */
int js_vm_eval_module(JSContext *ctx, const char *filename,
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

    /* Drive the event loop to completion so that top-level await
     * (e.g. await fetch()) resolves before we read exports. */
    js_loop_t *loop = JS_GetContextOpaque(ctx);
    if (loop) {
        js_loop_run(loop, JS_GetRuntime(ctx));
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

