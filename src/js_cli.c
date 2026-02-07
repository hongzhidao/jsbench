#include "js_main.h"

int js_cli_run(JSContext *ctx, js_config_t *config) {
    (void)config;

    /* In CLI mode, the module has already been evaluated.
     * We just need to run any pending async jobs until completion. */
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSContext *pctx;
    int ret;

    /* Keep running pending jobs until none remain */
    for (;;) {
        ret = JS_ExecutePendingJob(rt, &pctx);
        if (ret <= 0) break;
    }

    if (ret < 0) {
        JSValue exc = JS_GetException(pctx);
        const char *str = JS_ToCString(pctx, exc);
        if (str) {
            fprintf(stderr, "Error: %s\n", str);
            JS_FreeCString(pctx, str);
        }
        JS_FreeValue(pctx, exc);
        return 1;
    }

    /* Check if any unhandled promise rejections occurred during eval */
    if (js_had_unhandled_rejection)
        return 1;

    return 0;
}
