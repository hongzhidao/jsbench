#ifndef JS_EPOLL_H
#define JS_EPOLL_H

typedef struct js_engine_s js_engine_t;

typedef struct js_event_s  js_event_t;
typedef void (*js_event_handler_t)(js_event_t *ev);

struct js_event_s {
    int                   fd;
    void                 *data;
    js_event_handler_t    read;
    js_event_handler_t    write;
    js_event_handler_t    error;
};

int     js_epoll_add(js_engine_t *engine, js_event_t *ev, uint32_t events);
int     js_epoll_mod(js_engine_t *engine, js_event_t *ev, uint32_t events);
int     js_epoll_del(js_engine_t *engine, js_event_t *ev);
int     js_epoll_poll(js_engine_t *engine, int timeout_ms);

#endif /* JS_EPOLL_H */
