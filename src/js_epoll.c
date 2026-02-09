#include "js_main.h"

static __thread int js_epfd = -1;

int js_epoll_create(void) {
    js_epfd = epoll_create1(EPOLL_CLOEXEC);
    return js_epfd;
}

void js_epoll_close(void) {
    if (js_epfd >= 0) {
        close(js_epfd);
        js_epfd = -1;
    }
}

int js_epoll_add(js_event_t *ev, uint32_t events) {
    struct epoll_event e = {
        .events = events,
        .data.ptr = ev
    };
    return epoll_ctl(js_epfd, EPOLL_CTL_ADD, ev->fd, &e);
}

int js_epoll_mod(js_event_t *ev, uint32_t events) {
    struct epoll_event e = {
        .events = events,
        .data.ptr = ev
    };
    return epoll_ctl(js_epfd, EPOLL_CTL_MOD, ev->fd, &e);
}

int js_epoll_del(js_event_t *ev) {
    return epoll_ctl(js_epfd, EPOLL_CTL_DEL, ev->fd, NULL);
}

int js_epoll_poll(int timeout_ms) {
    struct epoll_event events[256];

    int n = epoll_wait(js_epfd, events, 256, timeout_ms);
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

int js_timerfd_create(double seconds) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) return -1;

    long sec = (long)seconds;
    long nsec = (long)((seconds - (double)sec) * 1e9);

    struct itimerspec ts = {
        .it_value = { .tv_sec = sec, .tv_nsec = nsec },
        .it_interval = { 0, 0 }
    };

    if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
        close(tfd);
        return -1;
    }

    return tfd;
}
