#include "js_main.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <script.js>\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "  Benchmark mode: script has 'export default' (URL/object/array/function)\n");
    fprintf(stderr, "  CLI mode:       script has no default export (runs as plain script)\n");
    fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *script_path = argv[1];

    /* Read script file */
    size_t source_len;
    char *source = js_read_file(script_path, &source_len);
    if (!source) {
        fprintf(stderr, "Error: cannot read file '%s': %s\n",
                script_path, strerror(errno));
        return 1;
    }

    /* Initialize QuickJS */
    JSContext *ctx = js_vm_create();
    if (!ctx) {
        fprintf(stderr, "Error: failed to create JS context\n");
        free(source);
        return 1;
    }

    /* Create engine and event loop (fetch() needs it during module evaluation) */
    js_engine_t *engine = js_engine_create();
    if (!engine) {
        fprintf(stderr, "Error: failed to create engine\n");
        js_vm_free(ctx);
        free(source);
        return 1;
    }
    js_thread()->engine = engine;

    js_loop_t *loop = js_loop_create();
    if (!loop) {
        fprintf(stderr, "Error: failed to create event loop\n");
        js_vm_free(ctx);
        js_engine_destroy(engine);
        free(source);
        return 1;
    }
    JS_SetContextOpaque(ctx, loop);

    /* Evaluate the module */
    JSValue default_export, bench_export;
    if (js_vm_eval_module(ctx, script_path, source, &default_export, &bench_export) != 0) {
        JS_SetContextOpaque(ctx, NULL);
        js_loop_free(loop);
        js_vm_free(ctx);
        free(source);
        return 1;
    }

    /* Detect mode */
    js_mode_t mode = js_vm_detect_mode(ctx, default_export);

    /* Build config */
    js_config_t config = {0};
    config.mode = mode;
    config.script_path = strdup(script_path);
    config.script_source = source;
    config.connections = 1;
    config.threads = 1;
    config.duration_sec = 0;

    int ret = 0;

    if (mode == MODE_CLI) {
        /* CLI mode: just run pending async jobs */
        ret = js_cli_run(ctx, &config);
    } else {
        /* Benchmark mode: extract config and requests */
        js_vm_extract_config(ctx, bench_export, &config);

        if (mode != MODE_BENCH_ASYNC) {
            /* Extract and serialize requests for C-path */
            if (js_vm_extract_requests(ctx, default_export, &config) != 0) {
                fprintf(stderr, "Error: failed to extract request configuration\n");
                ret = 1;
                goto cleanup;
            }

            if (config.request_count == 0) {
                fprintf(stderr, "Error: no valid requests found\n");
                ret = 1;
                goto cleanup;
            }
        } else {
            /* For async mode, we need at least a dummy request for DNS resolution */
            /* The actual requests happen in JS */
            /* We need to figure out the target URL for DNS */
            if (!config.target) {
                fprintf(stderr, "Error: async function mode requires 'target' in bench config,\n"
                               "       or the function must use full URLs in fetch() calls.\n"
                               "       Proceeding with localhost assumption...\n");
            }

            /* Create a minimal request entry for the orchestrator */
            config.request_count = 1;
            config.requests = calloc(1, sizeof(js_buf_t));
            if (config.target) {
                js_parse_url(config.target, &config.url);
                config.use_tls = config.url.is_tls;
            }
        }

        if (config.duration_sec <= 0) {
            config.duration_sec = 10.0;  /* default 10 seconds */
        }

        config.mode = mode;
        ret = js_bench_run(&config);
    }

cleanup:
    /* Free resources */
    JS_SetContextOpaque(ctx, NULL);
    js_loop_free(loop);
    JS_FreeValue(ctx, default_export);
    JS_FreeValue(ctx, bench_export);
    js_vm_free(ctx);
    js_engine_destroy(engine);

    for (int i = 0; i < config.request_count; i++) {
        js_buf_free(&config.requests[i]);
    }
    free(config.requests);
    free(config.target);
    free(config.host);
    free(config.script_path);
    free(config.script_source);

    return ret;
}
