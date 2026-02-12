#ifndef JS_FETCH_H
#define JS_FETCH_H

void    js_headers_init(JSContext *ctx);
JSValue js_headers_from_http(JSContext *ctx, const js_http_response_t *parsed);

void    js_response_init(JSContext *ctx);

#endif /* JS_FETCH_H */
