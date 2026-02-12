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
#include "js_tls.h"
#include "js_conn.h"
#include "js_http.h"
#include "js_fetch.h"
#include "js_stats.h"

/* ── Constants ────────────────────────────────────────────────────────── */

#define JS_MAX_CONNECTIONS  65536
#define JS_MAX_THREADS      256
#define JS_READ_BUF_SIZE    16384
#define JS_MAX_URL_LEN      4096

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

/* ── Pending operation (generic event loop node) ─────────────────────── */

typedef struct js_pending js_pending_t;

struct js_pending {
    js_loop_t           *loop;
    struct list_head     link;
    void               (*destroy)(js_pending_t *p);
};

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



/* loop.c */
js_loop_t  *js_loop_create(void);
void        js_loop_free(js_loop_t *loop);
int         js_loop_add(js_loop_t *loop, js_pending_t *p);
int         js_loop_run(js_loop_t *loop, JSRuntime *rt);

/* response.c */
JSValue js_response_new(JSContext *ctx, int status, const char *status_text,
                        const char *body, size_t body_len,
                        const js_http_response_t *parsed);

/* vm.c */
JSContext  *js_vm_create(void);
void        js_vm_free(JSContext *ctx);
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

/* worker.c */
void   *js_worker_run(void *arg);

#include "js_runtime.h"

#endif /* JS_H */
