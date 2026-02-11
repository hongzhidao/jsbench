#include "js_main.h"

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
