#ifndef JS_RBTREE_H
#define JS_RBTREE_H

typedef struct js_rbtree_node_s js_rbtree_node_t;

struct js_rbtree_node_s {
    js_rbtree_node_t *left;
    js_rbtree_node_t *right;
    js_rbtree_node_t *parent;
    uint8_t color;
};

typedef struct {
    js_rbtree_node_t sentinel;
} js_rbtree_t;

/*
 * A comparison function should return intptr_t result because
 * this eliminates overhead required to implement correct addresses
 * comparison without result truncation.
 */
typedef intptr_t (*js_rbtree_compare_t)(js_rbtree_node_t *node1,
    js_rbtree_node_t *node2);

#define js_rbtree_root(tree)                                                  \
    ((tree)->sentinel.left)

#define js_rbtree_sentinel(tree)                                              \
    (&(tree)->sentinel)

#define js_rbtree_is_empty(tree)                                              \
    (js_rbtree_root(tree) == js_rbtree_sentinel(tree))

#define js_rbtree_min(tree)                                                   \
    js_rbtree_branch_min(tree, &(tree)->sentinel)

static inline js_rbtree_node_t *
js_rbtree_branch_min(js_rbtree_t *tree, js_rbtree_node_t *node)
{
    while (node->left != js_rbtree_sentinel(tree)) {
        node = node->left;
    }

    return node;
}

#define js_rbtree_is_there_successor(tree, node)                              \
    ((node) != js_rbtree_sentinel(tree))

static inline js_rbtree_node_t *
js_rbtree_node_successor(js_rbtree_t *tree, js_rbtree_node_t *node)
{
    js_rbtree_node_t  *parent;

    if (node->right != js_rbtree_sentinel(tree)) {
        return js_rbtree_branch_min(tree, node->right);
    }

    for ( ;; ) {
        parent = node->parent;

        /*
         * Explicit test for a root node is not required here, because
         * the root node is always the left child of the sentinel.
         */
        if (node == parent->left) {
            return parent;
        }

        node = parent;
    }
}

void js_rbtree_init(js_rbtree_t *tree, js_rbtree_compare_t compare);
void js_rbtree_insert(js_rbtree_t *tree, js_rbtree_node_t *new_node);
void js_rbtree_delete(js_rbtree_t *tree, js_rbtree_node_t *node);

#endif /* JS_RBTREE_H */
