#ifndef JS_RUNTIME_H
#define JS_RUNTIME_H

/* ── Constants ────────────────────────────────────────────────────────── */

#define JS_MAX_CONNECTIONS  65536
#define JS_MAX_THREADS      256
#define JS_READ_BUF_SIZE    16384

/* ── Benchmark mode ───────────────────────────────────────────────────── */

typedef enum {
    MODE_CLI,            /* No default export: run as script */
    MODE_BENCH_STRING,   /* default export is a URL string */
    MODE_BENCH_OBJECT,   /* default export is a request descriptor */
    MODE_BENCH_ARRAY,    /* default export is an array */
    MODE_BENCH_ASYNC     /* default export is an async function */
} js_mode_t;

/* ── Benchmark configuration ──────────────────────────────────────────── */

typedef struct {
    /* From script */
    int         connections;
    int         threads;
    double      duration_sec;
    char       *target;          /* Override base URL */
    char       *host;            /* Override Host header */

    /* Resolved */
    js_mode_t  mode;
    char       *script_path;
    char       *script_source;

    /* Target URL (from first request) */
    js_url_t   url;

    /* Pre-built requests (C-path) */
    js_buf_t   *requests;
    int         request_count;

    /* Resolved address */
    struct sockaddr_storage addr;
    socklen_t               addr_len;

    /* TLS */
    bool        use_tls;
    SSL_CTX    *ssl_ctx;
} js_config_t;

/* ── Worker thread context ────────────────────────────────────────────── */

typedef struct {
    int             id;
    int             conn_count;      /* connections assigned to this worker */
    js_config_t   *config;
    js_stats_t     stats;
    pthread_t       thread;
    atomic_bool     stop;
} js_worker_t;

js_mode_t   js_runtime_detect_mode(JSContext *ctx, JSValue default_export);
int         js_runtime_extract_config(JSContext *ctx, JSValue bench_export,
                                       js_config_t *config);
int         js_runtime_extract_requests(JSContext *ctx, JSValue default_export,
                                         js_config_t *config);

int   js_bench_run(js_config_t *config);
void *js_worker_run(void *arg);

#endif /* JS_RUNTIME_H */
