#ifndef JS_CLANG_H
#define JS_CLANG_H

#define js_container_of(p, type, field) \
    ((type *) ((char *) (p) - offsetof(type, field)))

#endif /* JS_CLANG_H */
