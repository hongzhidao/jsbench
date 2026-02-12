# Encapsulating Complexity: A Technique That Works Every Time

You don't need many architecture techniques. One good one is enough — just keep using it.

This article does one thing: move fetch's complexity out of the event loop and back into fetch itself. Split a 529-line file, move logic that doesn't belong in loop, sever the last type dependency. Result: loop goes from 224 lines to 85, without losing a single feature.

This isn't the first time we've used this technique. Article five used it to encapsulate the event engine — callers went from writing twenty lines to writing one. This article applies the same technique to fetch. **If the same technique works in different places, it's not a trick — it's a principle.**

## Whose Job Is Loop Doing

Let's look at the problem.

`js_loop.c` is the event loop — a scheduler. But open it up, and more than half of its 224 lines are doing someone else's work:

```c
static void loop_on_read(js_event_t *ev) {
    // ... read data, feed HTTP parser, check completion status ...
}

static void pending_complete(js_loop_t *loop, js_pending_t *p) {
    // ... build Response, resolve Promise, clean up resources ...
}

static void pending_fail(js_loop_t *loop, js_pending_t *p, const char *msg) {
    // ... reject Promise, clean up resources ...
}
```

HTTP parsing, Promise resolve/reject, connection cleanup — these are all fetch's job. **A scheduler doing the work of an HTTP client.**

Then look at `js_fetch.c`. 529 lines, but not because fetch is complex — three completely different concepts are crammed into one file: the Headers class implementation, the Response class implementation, and the fetch function. Each has its own type definitions and initialization logic, but they share a file scope with no isolation.

Two problems, one root cause: **complexity isn't where it should be.**

## Step One: Split the File — Give Each Concept a Boundary

In those 529 lines, Headers and Response are each independent JS classes — type definitions, methods, registration, self-contained. fetch only creates a Response at the very end. The dependency between them is weak.

Split into three files:

- `js_headers.c` (181 lines): the entire Headers implementation. The core type `js_headers_t` is file-internal — outside this file, nobody knows what it looks like
- `js_response.c` (142 lines): the entire Response implementation. `js_response_t` is likewise only visible inside its file
- `js_fetch.c`: just the core fetch logic

The public interface is consolidated in `js_fetch.h` — five lines of declarations:

```c
void    js_headers_init(JSContext *ctx);
JSValue js_headers_from_http(JSContext *ctx, const js_http_response_t *parsed);
void    js_response_init(JSContext *ctx);
```

C doesn't have `private` keywords for classes, but file scope is a natural encapsulation boundary. **The file itself is the module.** This isn't unique to C — Java and Go use packages, Python uses modules, TypeScript uses file exports. Different forms, same principle: give a concept a boundary, don't let its internals leak out.

Code change: [c3181c1](https://github.com/hongzhidao/jsbench/commit/c3181c1)

## Step Two: Move Behavior — Let Fetch Handle Its Own Business

Structure is split cleanly, but behavior is still scattered. `loop_on_read`, `loop_on_write`, `pending_complete`, `pending_fail` in loop — all fetch's work.

Move it back.

First, unify the cleanup path. Previously, complete and fail each had their own resource cleanup code, mostly duplicated. Introduce `js_fetch_destroy()` — one function for all paths:

```c
void js_fetch_destroy(js_fetch_t *f) {
    js_pending_t *p = &f->pending;
    JSContext *ctx = p->ctx;

    js_epoll_del(js_thread()->engine, &f->conn->socket);
    js_timer_delete(&js_thread()->engine->timers, &f->timer);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_http_response_free(&f->response);
    js_conn_free(f->conn);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    list_del(&p->link);
    free(f);
}
```

Then complete and fail become a clean two-step — do your thing, then call destroy:

```c
static void js_fetch_complete(js_fetch_t *f) {
    JSValue response = js_response_new(ctx, ...);
    JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, &response);
    js_fetch_destroy(f);
}

static void js_fetch_fail(js_fetch_t *f, const char *message) {
    JSValue err = JS_NewError(ctx);
    JS_Call(ctx, p->reject, JS_UNDEFINED, 1, &err);
    js_fetch_destroy(f);
}
```

Timeout handling goes from a dozen lines to one:

```c
static void js_fetch_timeout_handler(js_timer_t *timer, void *data) {
    js_pending_t *p = data;
    js_fetch_fail(js_fetch_from_pending(p), "Request timeout");
}
```

Event handlers move over too. Callbacks are bound when `js_fetch()` creates the connection:

```c
conn->socket.read  = js_fetch_on_read;
conn->socket.write = js_fetch_on_write;
conn->socket.error = js_fetch_on_error;
```

**The creator is the owner.** Whoever creates the connection is responsible for its event handling and lifecycle. This principle holds in any language — in React, whoever creates state manages it; in Go, whoever starts a goroutine is responsible for shutting it down.

Code change: [eb6a070](https://github.com/hongzhidao/jsbench/commit/eb6a070)

## Step Three: Sever the Last Dependency

Behavior has moved back, but loop still knows about `js_fetch_t` — `loop_free` calls `js_fetch_destroy(js_fetch_from_pending(p))`, `loop_add` does `js_epoll_add`. fetch's shadow still lingers in loop's code.

How do we make loop completely unaware of fetch? The answer is function pointers — polymorphism in C.

Add a `destroy` callback to `js_pending_t`:

```c
struct js_pending {
    /* ... */
    void (*destroy)(js_pending_t *p);
};
```

fetch registers its own destroy function at creation time:

```c
p->destroy = js_fetch_destroy;
```

loop just calls the callback during cleanup, without knowing what's on the other end:

```c
// before: loop knows about fetch
js_fetch_destroy(js_fetch_from_pending(p));

// after: loop only knows about pending
p->destroy(p);
```

At the same time, `js_fetch_t` moves from `js_main.h` into `js_fetch.c`, becoming a file-private type. epoll registration also moves from `loop_add` back to `js_fetch()`.

Now loop has zero fetch-related types, functions, or macros. **The last thread is cut.**

In object-oriented languages, this pattern is called an interface or abstract class — Go's `io.Closer`, Java's `AutoCloseable`. C lacks the syntactic sugar, but function pointers do the same thing: **the caller doesn't need to know the concrete type, only what it can do.** Loop doesn't need to know whether a pending operation is fetch or WebSocket — it just needs to know it has a `destroy`.

Code change: [88045f2](https://github.com/hongzhidao/jsbench/commit/88045f2)

## The Result

After all three steps, `js_loop.c` is 85 lines:

```
js_loop_create()   →  create pending list
js_loop_free()     →  iterate pending, call p->destroy(p)
js_loop_add()      →  add to list
js_loop_run()      →  drain JS job queue → check pending → epoll poll → fire timers
```

No HTTP parsing, no Promise operations, no connection state checks, **no fetch-related types whatsoever**. Pure scheduling. If WebSocket support is added tomorrow, loop doesn't change — the new protocol implements its own `destroy` callback, and loop schedules it just the same.

Before and after:

| | Before | After |
|---|---|---|
| js_fetch.c | 529 lines (three concepts mixed) | 326 lines (fetch logic + lifecycle) |
| js_headers.c | didn't exist | 181 lines (standalone module) |
| js_response.c | didn't exist | 142 lines (standalone module) |
| js_loop.c | 224 lines (scheduling + HTTP + Promise) | 85 lines (pure scheduling) |

Total lines: 753 to 734 — roughly the same. **But the distribution of complexity is completely different.** Each file does one thing, each module handles only its own complexity.

## Same Technique, Second Time Around

Comparing article five and this one:

| | Article 5: Encapsulating the Event Engine | This article: Encapsulating Fetch |
|---|---|---|
| Scattered complexity | epoll_wait duplicated in two files | fetch behavior scattered in loop |
| Encapsulated into | `js_epoll_poll()` | `js_fetch_complete/fail/on_read/on_write` |
| What got simpler | Callers went from 20 lines to 1 | loop went from 224 lines to 85 |

Same technique, same result. **The encapsulated module absorbs its own complexity; the surrounding modules shed the burden that was never theirs.** Three steps — establish boundaries, return behavior, sever dependencies — each one making the system a little clearer.

## Encapsulating Complexity — A Reusable Architecture Tool

If you take away just one thing from this article, take this tool.

**When to use it:**

- A module is doing work that isn't its own — a scheduler parsing HTTP, a controller querying the database
- A file contains multiple independently-changing concepts — Headers and fetch have no reason to be bundled together
- The same logic is duplicated across different paths — complete and fail each writing their own cleanup

**How to use it:**

Three actions, in order. **Establish boundaries** — give each concept its own scope, make internal details invisible to the outside. **Return behavior** — move logic back to the module it belongs to, let the creator manage the lifecycle. **Sever dependencies** — replace concrete type references with callbacks or interfaces, let modules communicate only through abstractions. Boundaries first, then behavior, then dependencies — the order matters. Moving behavior before boundaries are clear just relocates the mess.

**How to verify:**

- Can this module be unaware of that module's existence? (loop doesn't know fetch)
- If you add a new thing of the same kind, does existing code need to change? (add WebSocket, loop stays the same)
- Does each file change for only one reason?

These questions aren't specific to C or systems programming. **In any language, any project, when code feels impossible to change or every change ripples through the entire system, complexity is probably scattered in the wrong places.** Find it, move it back. It's just one technique, but you'll use it many times.

---

GitHub: https://github.com/hongzhidao/jsbench
