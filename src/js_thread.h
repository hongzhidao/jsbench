#ifndef JS_THREAD_H
#define JS_THREAD_H

typedef struct {
    js_engine_t  *engine;
} js_thread_t;

extern __thread js_thread_t  js_thread_ctx;

#define js_thread()  (&js_thread_ctx)

#endif /* JS_THREAD_H */
