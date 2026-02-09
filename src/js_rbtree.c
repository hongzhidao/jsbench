#include "js_main.h"

static void js_rbtree_insert_fixup(js_rbtree_node_t *node);
static void js_rbtree_delete_fixup(js_rbtree_t *tree,
    js_rbtree_node_t *node);
static inline void js_rbtree_left_rotate(js_rbtree_node_t *node);
static inline void js_rbtree_right_rotate(js_rbtree_node_t *node);
static inline void js_rbtree_parent_relink(js_rbtree_node_t *subst,
    js_rbtree_node_t *node);

#define JS_RBTREE_BLACK  0
#define JS_RBTREE_RED    1

#define js_rbtree_comparison_callback(tree)                                   \
    ((js_rbtree_compare_t) (tree)->sentinel.right)

void js_rbtree_init(js_rbtree_t *tree, js_rbtree_compare_t compare)
{
    /*
     * The sentinel is used as a leaf node sentinel and as a tree root
     * sentinel: it is a parent of a root node and the root node is
     * the left child of the sentinel.  Combining two sentinels in one
     * entry and the fact that the sentinel's left child is a root node
     * simplifies js_rbtree_node_successor() and eliminates explicit
     * root node test before or inside js_rbtree_min().
     */

    /* The root is empty. */
    tree->sentinel.left = &tree->sentinel;

    /*
     * The sentinel's right child is never used so
     * comparison callback can be safely stored here.
     */
    tree->sentinel.right = (void *) compare;

    /* The root and leaf sentinel must be black. */
    tree->sentinel.color = JS_RBTREE_BLACK;
}

void js_rbtree_insert(js_rbtree_t *tree, js_rbtree_node_t *new_node)
{
    js_rbtree_node_t *node, *sentinel, **child;
    js_rbtree_compare_t compare;

    node = js_rbtree_root(tree);
    sentinel = js_rbtree_sentinel(tree);

    new_node->left = sentinel;
    new_node->right = sentinel;
    new_node->color = JS_RBTREE_RED;

    compare = (js_rbtree_compare_t) tree->sentinel.right;
    child = &js_rbtree_root(tree);

    while (*child != sentinel) {
        node = *child;
        child = (compare(new_node, node) < 0) ? &node->left : &node->right;
    }

    *child = new_node;
    new_node->parent = node;

    js_rbtree_insert_fixup(new_node);

    node = js_rbtree_root(tree);
    node->color = JS_RBTREE_BLACK;
}

static void js_rbtree_insert_fixup(js_rbtree_node_t *node)
{
    js_rbtree_node_t *parent, *grandparent, *uncle;

    for ( ;; ) {
        parent = node->parent;

        /*
         * Testing whether a node is a tree root is not required here since
         * a root node's parent is the sentinel and it is always black.
         */
        if (parent->color == JS_RBTREE_BLACK) {
            return;
        }

        grandparent = parent->parent;

        if (parent == grandparent->left) {
            uncle = grandparent->right;

            if (uncle->color == JS_RBTREE_BLACK) {

                if (node == parent->right) {
                    node = parent;
                    js_rbtree_left_rotate(node);
                }

                /*
                 * js_rbtree_left_rotate() swaps parent and
                 * child whilst keeps grandparent the same.
                 */
                parent = node->parent;

                parent->color = JS_RBTREE_BLACK;
                grandparent->color = JS_RBTREE_RED;

                js_rbtree_right_rotate(grandparent);
                /*
                 * js_rbtree_right_rotate() does not change node->parent
                 * color which is now black, so testing color is not required
                 * to return from function.
                 */
                return;
            }

        } else {
            uncle = grandparent->left;

            if (uncle->color == JS_RBTREE_BLACK) {

                if (node == parent->left) {
                    node = parent;
                    js_rbtree_right_rotate(node);
                }

                /* See the comment in the symmetric branch above. */
                parent = node->parent;

                parent->color = JS_RBTREE_BLACK;
                grandparent->color = JS_RBTREE_RED;

                js_rbtree_left_rotate(grandparent);

                /* See the comment in the symmetric branch above. */
                return;
            }
        }

        uncle->color = JS_RBTREE_BLACK;
        parent->color = JS_RBTREE_BLACK;
        grandparent->color = JS_RBTREE_RED;

        node = grandparent;
    }
}

void js_rbtree_delete(js_rbtree_t *tree, js_rbtree_node_t *node)
{
    uint8_t color;
    js_rbtree_node_t *sentinel, *subst, *child;

    subst = node;
    sentinel = js_rbtree_sentinel(tree);

    if (node->left == sentinel) {
        child = node->right;

    } else if (node->right == sentinel) {
        child = node->left;

    } else {
        subst = js_rbtree_branch_min(tree, node->right);
        child = subst->right;
    }

    js_rbtree_parent_relink(child, subst);

    color = subst->color;

    if (subst != node) {
        /* Move the subst node to the deleted node position in the tree. */

        subst->color = node->color;

        subst->left = node->left;
        subst->left->parent = subst;

        subst->right = node->right;
        subst->right->parent = subst;

        js_rbtree_parent_relink(subst, node);
    }

    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;

    if (color == JS_RBTREE_BLACK) {
        js_rbtree_delete_fixup(tree, child);
    }
}

static void js_rbtree_delete_fixup(js_rbtree_t *tree, js_rbtree_node_t *node)
{
    js_rbtree_node_t *parent, *sibling;

    while (node != js_rbtree_root(tree) && node->color == JS_RBTREE_BLACK) {

        parent = node->parent;

        if (node == parent->left) {
            sibling = parent->right;

            if (sibling->color != JS_RBTREE_BLACK) {

                sibling->color = JS_RBTREE_BLACK;
                parent->color = JS_RBTREE_RED;

                js_rbtree_left_rotate(parent);

                sibling = parent->right;
            }

            if (sibling->right->color == JS_RBTREE_BLACK) {

                sibling->color = JS_RBTREE_RED;

                if (sibling->left->color == JS_RBTREE_BLACK) {
                    node = parent;
                    continue;
                }

                sibling->left->color = JS_RBTREE_BLACK;

                js_rbtree_right_rotate(sibling);
                /*
                 * If the node is the leaf sentinel then the right
                 * rotate above changes its parent so a sibling below
                 * becames the leaf sentinel as well and this causes
                 * segmentation fault.  This is the reason why usual
                 * red-black tree implementations with a leaf sentinel
                 * which does not require to test leaf nodes at all
                 * nevertheless test the leaf sentinel in the left and
                 * right rotate procedures.  Since according to the
                 * algorithm node->parent must not be changed by both
                 * the left and right rotates above, it can be cached
                 * in a local variable.  This not only eliminates the
                 * sentinel test in js_rbtree_parent_relink() but also
                 * decreases the code size because C forces to reload
                 * non-restrict pointers.
                 */
                sibling = parent->right;
            }

            sibling->color = parent->color;
            parent->color = JS_RBTREE_BLACK;
            sibling->right->color = JS_RBTREE_BLACK;

            js_rbtree_left_rotate(parent);

            return;

        } else {
            sibling = parent->left;

            if (sibling->color != JS_RBTREE_BLACK) {

                sibling->color = JS_RBTREE_BLACK;
                parent->color = JS_RBTREE_RED;

                js_rbtree_right_rotate(parent);

                sibling = parent->left;
            }

            if (sibling->left->color == JS_RBTREE_BLACK) {

                sibling->color = JS_RBTREE_RED;

                if (sibling->right->color == JS_RBTREE_BLACK) {
                    node = parent;
                    continue;
                }

                sibling->right->color = JS_RBTREE_BLACK;

                js_rbtree_left_rotate(sibling);

                /* See the comment in the symmetric branch above. */
                sibling = parent->left;
            }

            sibling->color = parent->color;
            parent->color = JS_RBTREE_BLACK;
            sibling->left->color = JS_RBTREE_BLACK;

            js_rbtree_right_rotate(parent);

            return;
        }
    }

    node->color = JS_RBTREE_BLACK;
}

static inline void js_rbtree_left_rotate(js_rbtree_node_t *node)
{
    js_rbtree_node_t *child;

    child = node->right;
    node->right = child->left;
    child->left->parent = node;
    child->left = node;

    js_rbtree_parent_relink(child, node);

    node->parent = child;
}

static inline void js_rbtree_right_rotate(js_rbtree_node_t *node)
{
    js_rbtree_node_t *child;

    child = node->left;
    node->left = child->right;
    child->right->parent = node;
    child->right = node;

    js_rbtree_parent_relink(child, node);

    node->parent = child;
}

/* Relink a parent from the node to the subst node. */

static inline void js_rbtree_parent_relink(js_rbtree_node_t *subst, js_rbtree_node_t *node)
{
    js_rbtree_node_t *parent, **link;

    parent = node->parent;
    /*
     * The leaf sentinel's parent can be safely changed here.
     * See the comment in js_rbtree_delete_fixup() for details.
     */
    subst->parent = parent;
    /*
     * If the node's parent is the root sentinel it is safely changed
     * because the root sentinel's left child is the tree root.
     */
    link = (node == parent->left) ? &parent->left : &parent->right;
    *link = subst;
}
