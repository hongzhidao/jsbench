#include "js_main.h"

static intptr_t js_timer_rbtree_compare(js_rbtree_node_t *node1,
    js_rbtree_node_t *node2)
{
    js_timer_t *timer1, *timer2;

    timer1 = (js_timer_t *) node1;
    timer2 = (js_timer_t *) node2;

    /*
     * Timer values are distributed in small range, usually several minutes
     * and overflow every 49 days if js_msec_t is stored in 32 bits.
     * This signed comparison takes into account that overflow.
     */
    return (js_msec_int_t) (timer1->time - timer2->time);
}

void js_timers_init(js_timers_t *timers)
{
    js_rbtree_init(&timers->tree, js_timer_rbtree_compare);
    timers->now = 0;
    timers->minimum = 0;
}

void js_timer_add(js_timers_t *timers, js_timer_t *timer, js_msec_t timeout)
{
    int32_t diff;
    js_msec_t time;

    time = timers->now + timeout;

    timer->enabled = 1;

    if (js_timer_is_in_tree(timer)) {
        diff = (js_msec_int_t) (time - timer->time);

        if (diff >= -(int32_t)timer->bias && diff <= (int32_t)timer->bias) {
            return;
        }

        js_rbtree_delete(&timers->tree, &timer->node);
        js_timer_in_tree_clear(timer);
    }

    timer->time = time;
    js_rbtree_insert(&timers->tree, &timer->node);
}

void js_timer_delete(js_timers_t *timers, js_timer_t *timer)
{
    timer->enabled = 0;

    if (js_timer_is_in_tree(timer)) {
        js_rbtree_delete(&timers->tree, &timer->node);
        js_timer_in_tree_clear(timer);
    }
}

js_msec_t js_timer_find(js_timers_t *timers)
{
    int32_t delta;
    js_msec_t time;
    js_timer_t *timer;
    js_rbtree_t *tree;
    js_rbtree_node_t *node, *next;

    tree = &timers->tree;

    for (node = js_rbtree_min(tree);
         js_rbtree_is_there_successor(tree, node);
         node = next)
    {
        next = js_rbtree_node_successor(tree, node);

        timer = (js_timer_t *) node;

        if (timer->enabled) {
            time = timer->time;
            timers->minimum = time - timer->bias;

            delta = (js_msec_int_t) (time - timers->now);

            return (js_msec_t) (delta > 0 ? delta : 0);
        }
    }

    timers->minimum = timers->now + 24 * 60 * 60 * 1000;

    return (js_msec_t) -1;
}

void js_timer_expire(js_timers_t *timers, js_msec_t now)
{
    js_timer_t *timer;
    js_rbtree_t *tree;
    js_rbtree_node_t *node, *next;

    timers->now = now;

    if ((js_msec_int_t) (timers->minimum - now) > 0) {
        return;
    }

    tree = &timers->tree;

    for (node = js_rbtree_min(tree);
         js_rbtree_is_there_successor(tree, node);
         node = next)
    {
        timer = (js_timer_t *) node;

        if ((js_msec_int_t) (timer->time - now) > (int32_t) timer->bias) {
            return;
        }

        next = js_rbtree_node_successor(tree, node);

        js_rbtree_delete(tree, &timer->node);
        js_timer_in_tree_clear(timer);

        if (timer->enabled) {
            timer->enabled = 0;
            timer->handler(timer, timer->data);
        }
    }
}
