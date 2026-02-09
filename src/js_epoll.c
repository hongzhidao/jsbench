#include "js_main.h"

int js_epoll_create(void) {
    return epoll_create1(EPOLL_CLOEXEC);
}

int js_epoll_add(int epfd, js_event_t *ev, uint32_t events) {
    struct epoll_event e = {
        .events = events,
        .data.ptr = ev
    };
    return epoll_ctl(epfd, EPOLL_CTL_ADD, ev->fd, &e);
}

int js_epoll_mod(int epfd, js_event_t *ev, uint32_t events) {
    struct epoll_event e = {
        .events = events,
        .data.ptr = ev
    };
    return epoll_ctl(epfd, EPOLL_CTL_MOD, ev->fd, &e);
}

int js_epoll_del(int epfd, js_event_t *ev) {
    return epoll_ctl(epfd, EPOLL_CTL_DEL, ev->fd, NULL);
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
