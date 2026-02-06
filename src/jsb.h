#ifndef JSB_H
#define JSB_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <math.h>
#include <time.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "quickjs.h"

/* ── Constants ────────────────────────────────────────────────────────── */

#define JSB_MAX_CONNECTIONS  65536
#define JSB_MAX_THREADS      256
#define JSB_READ_BUF_SIZE    16384
#define JSB_MAX_HEADERS      64
#define JSB_MAX_URL_LEN      4096

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
    char    path[JSB_MAX_URL_LEN];
    bool    is_tls;
} jsb_url_t;

/* ── HTTP request descriptor ──────────────────────────────────────────── */

typedef struct {
    char   *url;
    char   *method;         /* GET, POST, etc. */
    char   *headers;        /* "Key: Value\r\n..." */
    char   *body;
    size_t  body_len;
} jsb_request_desc_t;

/* ── Pre-serialized HTTP request ──────────────────────────────────────── */

typedef struct {
    char   *data;
    size_t  len;
    jsb_url_t url;
} jsb_raw_request_t;

/* ── Histogram ────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t  slots[HIST_TOTAL_SLOTS];
    uint64_t  over;          /* > 1s */
    uint64_t  count;
    double    sum;           /* sum of all latencies in us */
    double    sum_sq;        /* sum of squares */
    double    min_val;
    double    max_val;
} jsb_hist_t;

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
    jsb_hist_t latency;
} jsb_stats_t;

/* ── HTTP response parser ─────────────────────────────────────────────── */

typedef enum {
    HTTP_PARSE_STATUS_LINE,
    HTTP_PARSE_HEADER_LINE,
    HTTP_PARSE_BODY_IDENTITY,
    HTTP_PARSE_BODY_CHUNKED,
    HTTP_PARSE_CHUNK_SIZE,
    HTTP_PARSE_CHUNK_DATA,
    HTTP_PARSE_CHUNK_TRAILER,
    HTTP_PARSE_DONE,
    HTTP_PARSE_ERROR
} http_parse_state_t;

typedef struct {
    char    name[128];
    char    value[4096];
} jsb_header_t;

typedef struct {
    http_parse_state_t state;
    int                status_code;
    char               status_text[64];

    jsb_header_t       headers[JSB_MAX_HEADERS];
    int                header_count;

    /* Body handling */
    char              *body;
    size_t             body_len;
    size_t             body_cap;
    size_t             content_length;
    bool               chunked;

    /* Chunk parsing state */
    size_t             chunk_remaining;

    /* Internal parse buffer */
    char              *buf;
    size_t             buf_len;
    size_t             buf_cap;
} jsb_http_response_t;

/* ── Connection state ─────────────────────────────────────────────────── */

typedef enum {
    CONN_CONNECTING,
    CONN_TLS_HANDSHAKE,
    CONN_WRITING,
    CONN_READING,
    CONN_DONE,
    CONN_ERROR
} conn_state_t;

typedef struct jsb_conn {
    int              fd;
    conn_state_t     state;
    SSL             *ssl;

    /* Request data */
    const char      *req_data;
    size_t           req_len;
    size_t           req_sent;

    /* Response parser */
    jsb_http_response_t response;

    /* Timing */
    uint64_t         start_ns;

    /* For round-robin in array mode */
    int              req_index;

    /* User data (for JS callbacks etc.) */
    void            *udata;
} jsb_conn_t;

/* ── Benchmark mode ───────────────────────────────────────────────────── */

typedef enum {
    MODE_CLI,            /* No default export: run as script */
    MODE_BENCH_STRING,   /* default export is a URL string */
    MODE_BENCH_OBJECT,   /* default export is a request descriptor */
    MODE_BENCH_ARRAY,    /* default export is an array */
    MODE_BENCH_ASYNC     /* default export is an async function */
} jsb_mode_t;

/* ── Benchmark configuration ──────────────────────────────────────────── */

typedef struct {
    /* From script */
    int         connections;
    int         threads;
    double      duration_sec;
    char       *target;          /* Override base URL */
    char       *host;            /* Override Host header */

    /* Resolved */
    jsb_mode_t  mode;
    char       *script_path;
    char       *script_source;

    /* Pre-built requests (C-path) */
    jsb_raw_request_t *requests;
    int                request_count;

    /* Resolved address */
    struct sockaddr_storage addr;
    socklen_t               addr_len;

    /* TLS */
    bool        use_tls;
    SSL_CTX    *ssl_ctx;
} jsb_config_t;

/* ── Worker thread context ────────────────────────────────────────────── */

typedef struct {
    int             id;
    int             conn_count;      /* connections assigned to this worker */
    jsb_config_t   *config;
    jsb_stats_t     stats;
    pthread_t       thread;
    atomic_bool     stop;
} jsb_worker_t;

/* ── Function declarations ────────────────────────────────────────────── */

/* util.c */
int     jsb_parse_url(const char *url_str, jsb_url_t *out);
double  jsb_parse_duration(const char *s);
char   *jsb_read_file(const char *path, size_t *len);
void    jsb_format_bytes(uint64_t bytes, char *buf, size_t buf_len);
void    jsb_format_duration(double us, char *buf, size_t buf_len);
int     jsb_set_nonblocking(int fd);
uint64_t jsb_now_ns(void);
int     jsb_serialize_request(const jsb_request_desc_t *desc,
                              const jsb_url_t *url,
                              const char *host_override,
                              jsb_raw_request_t *out);

/* stats.c */
void    jsb_hist_init(jsb_hist_t *h);
void    jsb_hist_add(jsb_hist_t *h, double us);
void    jsb_hist_merge(jsb_hist_t *dst, const jsb_hist_t *src);
double  jsb_hist_percentile(const jsb_hist_t *h, double p);
double  jsb_hist_mean(const jsb_hist_t *h);
double  jsb_hist_stdev(const jsb_hist_t *h);
void    jsb_stats_init(jsb_stats_t *s);
void    jsb_stats_merge(jsb_stats_t *dst, const jsb_stats_t *src);
void    jsb_stats_print(const jsb_stats_t *s, double duration_sec);

/* http_parser.c */
void    jsb_http_response_init(jsb_http_response_t *r);
void    jsb_http_response_free(jsb_http_response_t *r);
void    jsb_http_response_reset(jsb_http_response_t *r);
int     jsb_http_response_feed(jsb_http_response_t *r, const char *data, size_t len);
const char *jsb_http_response_header(const jsb_http_response_t *r, const char *name);

/* tls.c */
SSL_CTX *jsb_tls_ctx_create(void);
SSL     *jsb_tls_new(SSL_CTX *ctx, int fd, const char *hostname);
int      jsb_tls_handshake(SSL *ssl);
ssize_t  jsb_tls_read(SSL *ssl, void *buf, size_t len);
ssize_t  jsb_tls_write(SSL *ssl, const void *buf, size_t len);
void     jsb_tls_free(SSL *ssl);

/* event_loop.c */
int     jsb_epoll_create(void);
int     jsb_epoll_add(int epfd, int fd, uint32_t events, void *ptr);
int     jsb_epoll_mod(int epfd, int fd, uint32_t events, void *ptr);
int     jsb_epoll_del(int epfd, int fd);
int     jsb_timerfd_create(double seconds);

/* http_client.c */
jsb_conn_t *jsb_conn_create(const struct sockaddr *addr, socklen_t addr_len,
                             SSL_CTX *ssl_ctx, const char *hostname);
void        jsb_conn_free(jsb_conn_t *c);
int         jsb_conn_set_request(jsb_conn_t *c, const char *data, size_t len);
int         jsb_conn_handle_event(jsb_conn_t *c, uint32_t events);
void        jsb_conn_reset(jsb_conn_t *c, const struct sockaddr *addr,
                           socklen_t addr_len, SSL_CTX *ssl_ctx,
                           const char *hostname);
bool        jsb_conn_keepalive(const jsb_conn_t *c);
void        jsb_conn_reuse(jsb_conn_t *c);

/* fetch.c */
void    jsb_fetch_init(JSContext *ctx);

/* vm.c */
JSRuntime *jsb_vm_rt_create(void);
JSContext  *jsb_vm_ctx_create(JSRuntime *rt);
int         jsb_vm_eval_module(JSContext *ctx, const char *filename,
                               const char *source, JSValue *default_export,
                               JSValue *bench_export);
int         jsb_vm_extract_config(JSContext *ctx, JSValue bench_export,
                                  jsb_config_t *config);
int         jsb_vm_extract_requests(JSContext *ctx, JSValue default_export,
                                    jsb_config_t *config);
jsb_mode_t  jsb_vm_detect_mode(JSContext *ctx, JSValue default_export);
void        jsb_vm_setup_console(JSContext *ctx);

/* vm.c */
extern int jsb_had_unhandled_rejection;

/* cli.c */
int     jsb_cli_run(JSContext *ctx, jsb_config_t *config);

/* worker.c */
void   *jsb_worker_run(void *arg);

/* bench.c */
int     jsb_bench_run(jsb_config_t *config);

#endif /* JSB_H */
