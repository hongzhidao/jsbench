#include "js_main.h"

int js_epoll_add(js_engine_t *engine, js_event_t *ev, uint32_t events) {
    struct epoll_event e = {
        .events = events,
        .data.ptr = ev
    };
    return epoll_ctl(engine->epfd, EPOLL_CTL_ADD, ev->fd, &e);
}

int js_epoll_mod(js_engine_t *engine, js_event_t *ev, uint32_t events) {
    struct epoll_event e = {
        .events = events,
        .data.ptr = ev
    };
    return epoll_ctl(engine->epfd, EPOLL_CTL_MOD, ev->fd, &e);
}

int js_epoll_del(js_engine_t *engine, js_event_t *ev) {
    return epoll_ctl(engine->epfd, EPOLL_CTL_DEL, ev->fd, NULL);
}

int js_epoll_poll(js_engine_t *engine, int timeout_ms) {
    struct epoll_event events[256];

    int n = epoll_wait(engine->epfd, events, 256, timeout_ms);
    if (n < 0) {
        return (errno == EINTR) ? 0 : -1;
    }

    for (int i = 0; i < n; i++) {
        js_event_t *ev = events[i].data.ptr;
        uint32_t e = events[i].events;

        if (e & (EPOLLERR | EPOLLHUP)) {
            if (ev->error) ev->error(ev);
        } else {
            if ((e & EPOLLOUT) && ev->write) ev->write(ev);
            if ((e & EPOLLIN) && ev->read)   ev->read(ev);
        }
    }

    return 0;
}
