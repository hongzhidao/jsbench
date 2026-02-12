#ifndef JS_RUNTIME_H
#define JS_RUNTIME_H

js_mode_t   js_runtime_detect_mode(JSContext *ctx, JSValue default_export);
int         js_runtime_extract_config(JSContext *ctx, JSValue bench_export,
                                       js_config_t *config);
int         js_runtime_extract_requests(JSContext *ctx, JSValue default_export,
                                         js_config_t *config);

int js_bench_run(js_config_t *config);

#endif /* JS_RUNTIME_H */
