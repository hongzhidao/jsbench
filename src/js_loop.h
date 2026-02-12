#ifndef JS_LOOP_H
#define JS_LOOP_H

/* ── Event loop ──────────────────────────────────────────────────────── */

typedef struct js_loop js_loop_t;

/* ── Pending operation (generic event loop node) ─────────────────────── */

typedef struct js_pending js_pending_t;

struct js_pending {
    js_loop_t           *loop;
    struct list_head     link;
    void               (*destroy)(js_pending_t *p);
};

js_loop_t  *js_loop_create(void);
void        js_loop_free(js_loop_t *loop);
int         js_loop_add(js_loop_t *loop, js_pending_t *p);
int         js_loop_run(js_loop_t *loop, JSRuntime *rt);

#endif /* JS_LOOP_H */
