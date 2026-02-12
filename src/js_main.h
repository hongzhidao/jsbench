#ifndef JS_H
#define JS_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "list.h"
#include "cutils.h"
#include "quickjs.h"

#include "js_unix.h"
#include "js_clang.h"
#include "js_time.h"
#include "js_rbtree.h"
#include "js_epoll.h"
#include "js_timer.h"
#include "js_engine.h"
#include "js_thread.h"
#include "js_buf.h"
#include "js_conn.h"
#include "js_http.h"
#include "js_fetch.h"

/* ── Constants ────────────────────────────────────────────────────────── */

#define JS_MAX_CONNECTIONS  65536
#define JS_MAX_THREADS      256
#define JS_READ_BUF_SIZE    16384
#define JS_MAX_URL_LEN      4096

/* Histogram: 0-10ms at 1us resolution, 10ms-1s at 100us resolution */
#define HIST_FINE_SLOTS      10000   /* 0..9999 us  (0-10ms) */
#define HIST_COARSE_SLOTS    9900    /* 10000..999900 us (10ms-1s) */
#define HIST_TOTAL_SLOTS     (HIST_FINE_SLOTS + HIST_COARSE_SLOTS)
#define HIST_FINE_MAX_US     10000
#define HIST_COARSE_STEP     100

/* ── Parsed URL ───────────────────────────────────────────────────────── */

typedef struct {
    char    scheme[8];       /* "http" or "https" */
    char    host[256];
    char    port_str[8];
    int     port;
    char    path[JS_MAX_URL_LEN];
    bool    is_tls;
} js_url_t;

/* ── HTTP request ────────────────────────────────────────────────────── */

typedef struct {
    js_url_t     url;
    char        *method;       /* "GET", "POST", etc. */
    char        *headers;      /* "Key: Value\r\n..." */
    char        *body;
    size_t       body_len;
} js_request_t;

/* ── Histogram ────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t  slots[HIST_TOTAL_SLOTS];
    uint64_t  over;          /* > 1s */
    uint64_t  count;
    double    sum;           /* sum of all latencies in us */
    double    sum_sq;        /* sum of squares */
    double    min_val;
    double    max_val;
} js_hist_t;

/* ── Per-worker stats ─────────────────────────────────────────────────── */

typedef struct {
    uint64_t   requests;
    uint64_t   bytes_read;
    uint64_t   errors;
    uint64_t   connect_errors;
    uint64_t   read_errors;
    uint64_t   write_errors;
    uint64_t   timeout_errors;
    uint64_t   status_2xx;
    uint64_t   status_3xx;
    uint64_t   status_4xx;
    uint64_t   status_5xx;
    js_hist_t latency;
} js_stats_t;

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

/* ── Event loop ──────────────────────────────────────────────────────── */

typedef struct js_loop js_loop_t;

/* ── Pending operation (shared between loop and fetch) ───────────────── */

typedef struct {
    SSL_CTX             *ssl_ctx;
    JSContext           *ctx;
    JSValue              resolve;
    JSValue              reject;
    js_loop_t           *loop;
    struct list_head     link;
} js_pending_t;

/* ── Fetch context: what conn->socket.data points to ─────────────────── */

typedef struct {
    js_http_response_t   response;
    js_pending_t         pending;
    js_conn_t           *conn;
    js_timer_t           timer;
} js_fetch_t;

#define js_fetch_from_pending(p) \
    js_container_of(p, js_fetch_t, pending)

/* ── Function declarations ────────────────────────────────────────────── */

/* util.c */
int     js_parse_url(const char *url_str, js_url_t *out);
double  js_parse_duration(const char *s);
char   *js_read_file(const char *path, size_t *len);
void    js_format_bytes(uint64_t bytes, char *buf, size_t buf_len);
void    js_format_duration(double us, char *buf, size_t buf_len);
int     js_set_nonblocking(int fd);
uint64_t js_now_ns(void);
int     js_request_serialize(js_request_t *req, const char *host_override,
                              js_buf_t *out);
void    js_request_free(js_request_t *req);

/* stats.c */
void    js_hist_init(js_hist_t *h);
void    js_hist_add(js_hist_t *h, double us);
void    js_hist_merge(js_hist_t *dst, const js_hist_t *src);
double  js_hist_percentile(const js_hist_t *h, double p);
double  js_hist_mean(const js_hist_t *h);
double  js_hist_stdev(const js_hist_t *h);
void    js_stats_init(js_stats_t *s);
void    js_stats_merge(js_stats_t *dst, const js_stats_t *src);
void    js_stats_print(const js_stats_t *s, double duration_sec);

/* http_parser.c */
void    js_http_response_init(js_http_response_t *r);
void    js_http_response_free(js_http_response_t *r);
void    js_http_response_reset(js_http_response_t *r);
int     js_http_response_feed(js_http_response_t *r, const char *data, size_t len);
const char *js_http_response_header(const js_http_response_t *r, const char *name);

/* tls.c */
SSL_CTX *js_tls_ctx_create(void);
SSL     *js_tls_new(SSL_CTX *ctx, int fd, const char *hostname);
int      js_tls_handshake(SSL *ssl);
ssize_t  js_tls_read(SSL *ssl, void *buf, size_t len);
ssize_t  js_tls_write(SSL *ssl, const void *buf, size_t len);
void     js_tls_free(SSL *ssl);

/* http_client.c */
js_conn_t *js_conn_create(const struct sockaddr *addr, socklen_t addr_len,
                             SSL_CTX *ssl_ctx, const char *hostname);
void        js_conn_free(js_conn_t *c);
int         js_conn_set_output(js_conn_t *c, const char *data, size_t len);
void        js_conn_reset(js_conn_t *c, const struct sockaddr *addr,
                           socklen_t addr_len, SSL_CTX *ssl_ctx,
                           const char *hostname);
void        js_conn_reuse(js_conn_t *c);
void        js_conn_process_write(js_conn_t *c);

/* loop.c */
js_loop_t  *js_loop_create(void);
void        js_loop_free(js_loop_t *loop);
int         js_loop_add(js_loop_t *loop, js_pending_t *p);
int         js_loop_run(js_loop_t *loop, JSRuntime *rt);

/* response.c */
JSValue js_response_new(JSContext *ctx, int status, const char *status_text,
                        const char *body, size_t body_len,
                        const js_http_response_t *parsed);

/* fetch.c */
void    js_fetch_init(JSContext *ctx);

/* vm.c */
JSRuntime *js_vm_rt_create(void);
JSContext  *js_vm_ctx_create(JSRuntime *rt);
int         js_vm_eval_module(JSContext *ctx, const char *filename,
                               const char *source, JSValue *default_export,
                               JSValue *bench_export);
int         js_vm_extract_config(JSContext *ctx, JSValue bench_export,
                                  js_config_t *config);
int         js_vm_extract_requests(JSContext *ctx, JSValue default_export,
                                    js_config_t *config);
js_mode_t  js_vm_detect_mode(JSContext *ctx, JSValue default_export);
void        js_vm_setup_console(JSContext *ctx);

/* vm.c */
extern int js_had_unhandled_rejection;

/* cli.c */
int     js_cli_run(JSContext *ctx, js_config_t *config);

/* worker.c */
void   *js_worker_run(void *arg);

/* bench.c */
int     js_bench_run(js_config_t *config);

#endif /* JS_H */
