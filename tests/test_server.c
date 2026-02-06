#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <ctype.h>

#define MAX_EVENTS   64
#define BUF_SIZE     16384
#define MAX_HEADERS  32

/* ── Request parser (minimal) ─────────────────────────────────────────── */

typedef struct {
    char method[16];
    char path[1024];
    char headers[MAX_HEADERS][2][256];
    int  header_count;
    char *body;
    size_t body_len;
    size_t content_length;
    bool headers_done;
    bool complete;
} request_t;

static void request_init(request_t *r) {
    memset(r, 0, sizeof(*r));
}

/* ── Simple response helpers ──────────────────────────────────────────── */

static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body,
                          const char *extra_headers) {
    size_t body_len = body ? strlen(body) : 0;
    char header[2048];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        status, status_text, content_type, body_len,
        extra_headers ? extra_headers : "");

    write(fd, header, (size_t)hlen);
    if (body && body_len > 0)
        write(fd, body, body_len);
}

static void send_chunked_response(int fd) {
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";
    write(fd, header, strlen(header));

    /* Send 3 chunks */
    const char *chunks[] = { "Hello, ", "chunked ", "world!" };
    for (int i = 0; i < 3; i++) {
        char chunk_header[32];
        int cl = snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", strlen(chunks[i]));
        write(fd, chunk_header, (size_t)cl);
        write(fd, chunks[i], strlen(chunks[i]));
        write(fd, "\r\n", 2);
    }
    /* Final chunk */
    write(fd, "0\r\n\r\n", 5);
}

/* ── Route handling ───────────────────────────────────────────────────── */

static void handle_request(int fd, const char *buf, size_t buf_len) {
    request_t req;
    request_init(&req);

    /* Parse request line */
    const char *p = buf;
    const char *end = buf + buf_len;

    /* Method */
    const char *sp = memchr(p, ' ', (size_t)(end - p));
    if (!sp) { close(fd); return; }
    size_t mlen = (size_t)(sp - p);
    if (mlen >= sizeof(req.method)) mlen = sizeof(req.method) - 1;
    memcpy(req.method, p, mlen);
    req.method[mlen] = '\0';
    p = sp + 1;

    /* Path */
    sp = memchr(p, ' ', (size_t)(end - p));
    if (!sp) { close(fd); return; }
    size_t plen = (size_t)(sp - p);
    if (plen >= sizeof(req.path)) plen = sizeof(req.path) - 1;
    memcpy(req.path, p, plen);
    req.path[plen] = '\0';

    /* Find end of request line */
    const char *crlf = strstr(sp, "\r\n");
    if (!crlf) { close(fd); return; }
    p = crlf + 2;

    /* Parse headers */
    while (p < end - 1) {
        if (p[0] == '\r' && p[1] == '\n') {
            p += 2;
            break;  /* End of headers */
        }

        const char *line_end = strstr(p, "\r\n");
        if (!line_end) break;

        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon && req.header_count < MAX_HEADERS) {
            size_t nlen = (size_t)(colon - p);
            if (nlen >= 256) nlen = 255;
            memcpy(req.headers[req.header_count][0], p, nlen);
            req.headers[req.header_count][0][nlen] = '\0';

            const char *val = colon + 1;
            while (val < line_end && *val == ' ') val++;
            size_t vlen = (size_t)(line_end - val);
            if (vlen >= 256) vlen = 255;
            memcpy(req.headers[req.header_count][1], val, vlen);
            req.headers[req.header_count][1][vlen] = '\0';

            /* Check for Content-Length */
            if (strcasecmp(req.headers[req.header_count][0], "Content-Length") == 0)
                req.content_length = (size_t)atol(req.headers[req.header_count][1]);

            req.header_count++;
        }
        p = line_end + 2;
    }

    /* Body */
    req.body = (char *)p;
    req.body_len = (size_t)(end - p);

    /* Route dispatch */
    if (strcmp(req.path, "/health") == 0) {
        send_response(fd, 200, "OK", "text/plain", "OK", NULL);
    }
    else if (strcmp(req.path, "/json") == 0) {
        send_response(fd, 200, "OK", "application/json",
                      "{\"message\":\"hello\",\"number\":42}", NULL);
    }
    else if (strcmp(req.path, "/echo") == 0) {
        /* Echo back the request body */
        char resp_body[BUF_SIZE];
        size_t copy_len = req.body_len < sizeof(resp_body) - 1 ? req.body_len : sizeof(resp_body) - 1;
        memcpy(resp_body, req.body, copy_len);
        resp_body[copy_len] = '\0';
        send_response(fd, 200, "OK", "text/plain", resp_body, NULL);
    }
    else if (strcmp(req.path, "/headers") == 0) {
        /* Return request headers as JSON */
        char json[4096] = "{";
        int off = 1;
        for (int i = 0; i < req.header_count; i++) {
            if (i > 0) json[off++] = ',';
            off += snprintf(json + off, sizeof(json) - (size_t)off,
                           "\"%s\":\"%s\"",
                           req.headers[i][0], req.headers[i][1]);
        }
        json[off++] = '}';
        json[off] = '\0';
        send_response(fd, 200, "OK", "application/json", json, NULL);
    }
    else if (strncmp(req.path, "/status/", 8) == 0) {
        int code = atoi(req.path + 8);
        if (code < 100 || code > 599) code = 200;
        const char *text = "OK";
        if (code == 404) text = "Not Found";
        else if (code == 500) text = "Internal Server Error";
        else if (code == 301) text = "Moved Permanently";
        char body[64];
        snprintf(body, sizeof(body), "Status: %d", code);
        send_response(fd, code, text, "text/plain", body, NULL);
    }
    else if (strcmp(req.path, "/chunked") == 0) {
        send_chunked_response(fd);
    }
    else if (strcmp(req.path, "/large") == 0) {
        /* Return a larger response for bandwidth testing */
        char *body = malloc(10240);
        memset(body, 'X', 10240);
        body[10239] = '\0';
        send_response(fd, 200, "OK", "text/plain", body, NULL);
        free(body);
    }
    else {
        send_response(fd, 404, "Not Found", "text/plain", "Not Found", NULL);
    }

    close(fd);
}

/* ── Connection handler thread ────────────────────────────────────────── */

static void *handle_conn(void *arg) {
    int fd = (int)(long)arg;
    char buf[BUF_SIZE];
    size_t total = 0;

    /* Read until we have the full request (headers + body) */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';

        /* Check if we have complete headers */
        char *header_end = strstr(buf, "\r\n\r\n");
        if (header_end) {
            /* Check if we need to read more for body */
            char *cl = strcasestr(buf, "Content-Length:");
            if (cl) {
                size_t content_length = (size_t)atol(cl + 15);
                size_t header_len = (size_t)(header_end - buf) + 4;
                if (total >= header_len + content_length)
                    break;
            } else {
                break;
            }
        }
        if (total >= sizeof(buf) - 1) break;
    }

    if (total > 0)
        handle_request(fd, buf, total);
    else
        close(fd);

    return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

static volatile bool running = true;

static void sigint_handler(int sig) {
    (void)sig;
    running = false;
}

int main(int argc, char **argv) {
    int port = 18080;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return 1;
    }

    if (listen(srv, 128) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    fprintf(stderr, "Test server listening on port %d\n", port);
    fflush(stderr);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Use a timeout on accept to allow checking 'running' flag */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int client_fd = accept(srv, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            if (!running) break;
            perror("accept");
            continue;
        }

        int tcp_one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &tcp_one, sizeof(tcp_one));

        /* Handle in a detached thread */
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &attr, handle_conn, (void *)(long)client_fd);
        pthread_attr_destroy(&attr);
    }

    close(srv);
    fprintf(stderr, "Test server stopped\n");
    return 0;
}
