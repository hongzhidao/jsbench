#ifndef JS_VM_H
#define JS_VM_H

JSContext  *js_vm_create(void);
void        js_vm_free(JSContext *ctx);
int         js_vm_eval_module(JSContext *ctx, const char *filename,
                               const char *source, JSValue *default_export,
                               JSValue *bench_export);
extern int js_had_unhandled_rejection;

#endif /* JS_VM_H */
