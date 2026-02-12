#ifndef JS_RUNTIME_H
#define JS_RUNTIME_H

int js_cli_run(JSContext *ctx, js_config_t *config);
int js_bench_run(js_config_t *config);

#endif /* JS_RUNTIME_H */
