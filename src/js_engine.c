#include "js_main.h"

js_engine_t *js_engine_create(void)
{
    js_engine_t *engine;

    engine = malloc(sizeof(js_engine_t));
    if (engine == NULL) {
        return NULL;
    }

    engine->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (engine->epfd < 0) {
        free(engine);
        return NULL;
    }

    js_timers_init(&engine->timers);
    engine->timers.now = (js_msec_t) (js_now_ns() / 1000000);

    return engine;
}

void js_engine_destroy(js_engine_t *engine)
{
    if (engine == NULL) {
        return;
    }

    if (engine->epfd >= 0) {
        close(engine->epfd);
    }

    free(engine);
}
