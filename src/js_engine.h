#ifndef JS_ENGINE_H
#define JS_ENGINE_H

typedef struct js_engine_s js_engine_t;

struct js_engine_s {
    int epfd;
    js_timers_t timers;
};

js_engine_t *js_engine_create(void);
void js_engine_destroy(js_engine_t *engine);

#endif /* JS_ENGINE_H */
