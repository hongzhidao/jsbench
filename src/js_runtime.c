#include "js_main.h"

/* ── Detect mode from default export ──────────────────────────────────── */

js_mode_t js_runtime_detect_mode(JSContext *ctx, JSValue default_export) {
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

int js_runtime_extract_config(JSContext *ctx, JSValue bench_export,
                               js_config_t *config) {
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
            config->duration_sec = js_parse_duration(s);
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
                                  js_config_t *config, const char *target_override) {
    js_request_t req = {0};

    if (JS_IsString(val)) {
        const char *s = JS_ToCString(ctx, val);
        if (!s) return -1;

        if (target_override) {
            js_url_t turl;
            if (js_parse_url(target_override, &turl) != 0) {
                JS_FreeCString(ctx, s);
                return -1;
            }
            if (s[0] == '/') {
                char full_url[JS_MAX_URL_LEN];
                snprintf(full_url, sizeof(full_url), "%s://%s:%d%s",
                         turl.scheme, turl.host, turl.port, s);
                if (js_parse_url(full_url, &req.url) != 0) {
                    JS_FreeCString(ctx, s);
                    return -1;
                }
            } else {
                if (js_parse_url(s, &req.url) != 0) {
                    JS_FreeCString(ctx, s);
                    return -1;
                }
                strcpy(req.url.host, turl.host);
                req.url.port = turl.port;
                strcpy(req.url.port_str, turl.port_str);
                req.url.is_tls = turl.is_tls;
                strcpy(req.url.scheme, turl.scheme);
            }
        } else {
            if (js_parse_url(s, &req.url) != 0) {
                JS_FreeCString(ctx, s);
                return -1;
            }
        }
        JS_FreeCString(ctx, s);
        req.method = strdup("GET");

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

        if (target_override && url_s[0] == '/') {
            js_url_t turl;
            if (js_parse_url(target_override, &turl) == 0) {
                char full_url[JS_MAX_URL_LEN];
                snprintf(full_url, sizeof(full_url), "%s://%s:%d%s",
                         turl.scheme, turl.host, turl.port, url_s);
                js_parse_url(full_url, &req.url);
            } else {
                js_parse_url(url_s, &req.url);
            }
        } else if (target_override) {
            js_parse_url(url_s, &req.url);
            js_url_t turl;
            if (js_parse_url(target_override, &turl) == 0) {
                strcpy(req.url.host, turl.host);
                req.url.port = turl.port;
                strcpy(req.url.port_str, turl.port_str);
                req.url.is_tls = turl.is_tls;
                strcpy(req.url.scheme, turl.scheme);
            }
        } else {
            js_parse_url(url_s, &req.url);
        }
        JS_FreeCString(ctx, url_s);

        if (JS_IsString(v_method)) {
            const char *m = JS_ToCString(ctx, v_method);
            req.method = strdup(m);
            JS_FreeCString(ctx, m);
        } else {
            req.method = strdup("GET");
        }

        if (JS_IsString(v_body)) {
            const char *b = JS_ToCString(ctx, v_body);
            req.body = strdup(b);
            req.body_len = strlen(b);
            JS_FreeCString(ctx, b);
        }

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
            if (off > 0) req.headers = strdup(hdr_buf);
        }

        JS_FreeValue(ctx, v_url);
        JS_FreeValue(ctx, v_method);
        JS_FreeValue(ctx, v_body);
        JS_FreeValue(ctx, v_headers);

    } else {
        return -1;
    }

    /* Grow buf array */
    js_buf_t *tmp = realloc(config->requests,
                            sizeof(js_buf_t) * (size_t)(config->request_count + 1));
    if (!tmp) {
        js_request_free(&req);
        return -1;
    }
    config->requests = tmp;

    js_buf_t *buf = &config->requests[config->request_count];
    memset(buf, 0, sizeof(*buf));

    if (js_request_serialize(&req, config->host, buf) != 0) {
        js_request_free(&req);
        return -1;
    }

    config->request_count++;

    if (config->request_count == 1) {
        config->url = req.url;
        config->use_tls = req.url.is_tls;
    }

    js_request_free(&req);
    return 0;
}

int js_runtime_extract_requests(JSContext *ctx, JSValue default_export,
                                 js_config_t *config) {
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

/* ── Benchmark mode ──────────────────────────────────────────────────── */

int js_bench_run(js_config_t *config) {
    int nthreads = config->threads;
    int nconns = config->connections;

    if (nthreads <= 0) nthreads = 1;
    if (nconns <= 0) nconns = 1;
    if (nthreads > nconns) nthreads = nconns;

    /* Resolve DNS once */
    js_url_t *first_url = &config->url;

    /* If target override, use that for DNS */
    js_url_t target_url;
    if (config->target) {
        if (js_parse_url(config->target, &target_url) == 0) {
            first_url = &target_url;
        }
    }

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int gai_err = getaddrinfo(first_url->host, first_url->port_str, &hints, &res);
    if (gai_err != 0 || !res) {
        fprintf(stderr, "DNS resolution failed for %s: %s\n",
                first_url->host, gai_strerror(gai_err));
        return 1;
    }

    memcpy(&config->addr, res->ai_addr, res->ai_addrlen);
    config->addr_len = res->ai_addrlen;
    freeaddrinfo(res);

    /* Create TLS context if needed */
    if (config->use_tls) {
        config->ssl_ctx = js_tls_ctx_create();
        if (!config->ssl_ctx) {
            fprintf(stderr, "Failed to create TLS context\n");
            return 1;
        }
    }

    /* Print benchmark info */
    printf("Running benchmark: %d connection(s), %d thread(s)",
           nconns, nthreads);
    if (config->duration_sec > 0)
        printf(", %.0fs duration", config->duration_sec);
    printf("\n");
    printf("Target: %s://%s:%d%s\n",
           config->url.scheme,
           config->url.host,
           config->url.port,
           config->url.path);
    if (config->mode == MODE_BENCH_ASYNC)
        printf("Mode: async function (JS path)\n");
    else if (config->mode == MODE_BENCH_ARRAY)
        printf("Mode: array round-robin (%d endpoints)\n", config->request_count);
    else
        printf("Mode: %s (C path)\n",
               config->mode == MODE_BENCH_STRING ? "string" : "object");
    printf("\n");

    /* Allocate workers */
    js_worker_t *workers = calloc((size_t)nthreads, sizeof(js_worker_t));

    /* Distribute connections across threads */
    int conns_per_thread = nconns / nthreads;
    int extra_conns = nconns % nthreads;

    for (int i = 0; i < nthreads; i++) {
        workers[i].id = i;
        workers[i].config = config;
        workers[i].conn_count = conns_per_thread + (i < extra_conns ? 1 : 0);
        atomic_init(&workers[i].stop, false);
    }

    /* Start timing */
    uint64_t start_ns = js_now_ns();

    /* Launch worker threads */
    for (int i = 0; i < nthreads; i++) {
        pthread_create(&workers[i].thread, NULL, js_worker_run, &workers[i]);
    }

    /* Wait for all workers */
    for (int i = 0; i < nthreads; i++) {
        pthread_join(workers[i].thread, NULL);
    }

    uint64_t end_ns = js_now_ns();
    double actual_duration = (double)(end_ns - start_ns) / 1e9;

    /* Aggregate stats */
    js_stats_t total;
    js_stats_init(&total);
    for (int i = 0; i < nthreads; i++) {
        js_stats_merge(&total, &workers[i].stats);
    }

    /* Print results */
    js_stats_print(&total, actual_duration);

    /* Cleanup */
    free(workers);
    if (config->ssl_ctx) {
        SSL_CTX_free(config->ssl_ctx);
        config->ssl_ctx = NULL;
    }

    return 0;
}
