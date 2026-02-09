#ifndef JS_TIMER_H
#define JS_TIMER_H

#define JS_TIMER_DEFAULT_BIAS  50

typedef struct js_timer_s js_timer_t;

typedef void (*js_timer_handler_t)(js_timer_t *timer, void *data);

struct js_timer_s {
    /* The rbtree node must be the first field. */
    js_rbtree_node_t node;
    uint8_t bias;
    uint8_t enabled;
    js_msec_t time;
    js_timer_handler_t handler;
    void *data;
};

typedef struct {
    js_rbtree_t tree;
    js_msec_t now;
    js_msec_t minimum;
} js_timers_t;

#define js_timer_data(obj, type, timer) \
    js_container_of(obj, type, timer)

#define js_timer_is_in_tree(timer) \
    ((timer)->node.parent != NULL)

#define js_timer_in_tree_clear(timer) \
    (timer)->node.parent = NULL

void js_timers_init(js_timers_t *timers);
js_msec_t js_timer_find(js_timers_t *timers);
void js_timer_expire(js_timers_t *timers, js_msec_t now);
void js_timer_add(js_timers_t *timers, js_timer_t *timer, js_msec_t timeout);
void js_timer_delete(js_timers_t *timers, js_timer_t *timer);

static inline void js_timer_disable(js_timer_t *timer)
{
    timer->enabled = 0;
}

#endif /* JS_TIMER_H */
