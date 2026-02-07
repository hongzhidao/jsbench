#include "js_main.h"

int js_cli_run(JSContext *ctx, js_config_t *config) {
    (void)config;

    js_loop_t *loop = JS_GetContextOpaque(ctx);
    return js_loop_run(loop, JS_GetRuntime(ctx));
}
