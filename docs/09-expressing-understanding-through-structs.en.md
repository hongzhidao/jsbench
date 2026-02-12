# Code Runs, but Three Concepts Are Missing

The code runs, tests pass, features work — but something feels off.

Every time I get that feeling and dig deeper, the root cause is the same: **a domain concept that should exist in the code, but doesn't.** Not a missing feature — features all work. A missing concept — the code doesn't know what it's operating on.

This article uses two examples to show how to find those missing concepts, and what happens to the code once you do.

## Who Uses conn

**The first: the C worker path.** The worker creates connections, sets up callbacks, reads data, feeds the HTTP parser, records stats, checks keep-alive, manages connection reuse. This path is straightforward.

**The second: the JS fetch path.** fetch parses the URL, resolves DNS, creates the connection, then calls `js_loop_add()` to hand it off to loop. From that moment on, fetch is done. Reading, writing, parsing, completion checks, timeout handling — it's all in loop.

The last article already said: loop shouldn't handle HTTP, fetch should. But before tackling that, let's look at the C path first. It's simpler, but has its own problem.

## The C Path: start_ns Doesn't Belong in conn

After a request completes, the worker calculates latency:

```c
uint64_t elapsed_ns = js_now_ns() - c->start_ns;
```

`start_ns` lives in `js_conn_t`. But request timing is an HTTP-layer concern, not a transport-layer concern. If conn does a keep-alive reconnect or TLS renegotiation, does that time count? The connection layer shouldn't know.

The deeper issue: the worker's callback gets the HTTP response from `c->socket.data` and timing from `c` — **one request's state is split across two places.**

There should be a struct expressing "the HTTP state of a peer interaction":

```c
typedef struct {
    js_http_response_t  response;
    uint64_t            start_ns;
} js_http_peer_t;
```

response and start_ns go together because they belong to the same concept: one request/response exchange. `start_ns` is removed from `js_conn_t` — conn gets a little cleaner.

At the same time, HTTP type definitions — `js_header_t`, `js_http_response_t`, `js_http_peer_t` — move from `js_main.h` to `js_http.h`. HTTP finally has its own header file.

Code change: [180ecbd](https://github.com/hongzhidao/jsbench/commit/180ecbd)

## The JS Path: fetch Doesn't Exist as a Concept in Code

Look at `js_loop_add()`'s signature:

```c
int js_loop_add(js_loop_t *loop, js_conn_t *conn,
                SSL_CTX *ssl_ctx, JSContext *ctx,
                JSValue resolve, JSValue reject);
```

Six parameters, passed in loose. loop allocates a struct internally, fills in these parameters, then initializes the HTTP response parser, sets up timeouts, registers with epoll.

An event loop doing HTTP client initialization. Why? Because **there's no struct in the code expressing what "a fetch operation" is.**

Once you understand what fetch is, three concepts emerge naturally:

**A fetch operation** — one complete HTTP request/response cycle. It owns the connection, the response parser, the timeout timer.

**A pending interface** — the minimum information loop needs to schedule a pending operation. Promise callbacks, a list node, a loop reference.

**A thread context** — engine is per-thread infrastructure. It shouldn't belong to loop or worker — it's a property of the thread itself.

Three concepts, three structs:

```c
typedef struct {
    js_engine_t  *engine;
} js_thread_t;

typedef struct {
    SSL_CTX             *ssl_ctx;
    JSContext           *ctx;
    JSValue              resolve;
    JSValue              reject;
    js_loop_t           *loop;
    struct list_head     link;
} js_pending_t;

typedef struct {
    js_http_response_t   response;
    js_pending_t         pending;
    js_conn_t           *conn;
    js_timer_t           timer;
} js_fetch_t;
```

Every field has a home. conn and timer belong to fetch, not loop. engine belongs to the thread, not loop. pending is loop's scheduling interface, containing only what loop needs.

With these structs, `js_loop_add()` goes from six parameters to two. Allocation and initialization naturally happen in `js_fetch()`. Timeout handling moves from loop back to fetch. `struct js_loop` shrinks from four fields to one — a single `struct list_head`.

This is a substantial change. The details are covered in the next article.

Code change: [3ec7f15](https://github.com/hongzhidao/jsbench/commit/3ec7f15)

## Structs Aren't Storage Containers

Both changes do the same thing: **recognizing a domain concept the code didn't express, and giving it a struct.**

- No "HTTP peer" → `start_ns` and `response` scattered across two places. `js_http_peer_t` gives them a home.
- No "fetch operation" → conn, timer, response mixed into loop. `js_fetch_t` gives them a home.
- No "thread context" → engine stored in both loop and worker. `js_thread_t` gives it a home.

**Which struct a field belongs to reflects your understanding of "who owns what."** When the understanding is right, fields are in the right place. When it's missing, fields scatter to where they don't belong. DDD says code should reflect your understanding of the problem domain — in C, that understanding ultimately manifests as struct definitions.

This also explains why AI doesn't tend to make these improvements proactively. AI's goal is "making it work," putting data wherever is most convenient — wherever it's needed. But "convenient" and "correct" aren't the same thing. `start_ns` in conn is convenient, but not correct. Fetch allocation in loop is convenient, but not correct. **Identifying domain concepts, judging ownership, making structural decisions — that's human work.**

## Where the Difficulty Lies

It's easy to say — find the right model. It's hard to do.

The difficulty is that "right" isn't obvious. `js_http_peer_t` looks simple in hindsight — response plus start_ns, of course they belong together. But before it existed, nobody felt that `start_ns` in conn was wrong. The code ran, tests passed, features worked. **The problem isn't "it's broken" — it's "not realizing it could be better."**

Even harder is identifying the truly core models. `js_fetch_t` isn't just moving fields around — it redefines the boundary between fetch and loop, changes allocation strategy, reassigns timeout handling, reshapes function signatures. Introducing one struct triggers a cascade of changes. **A good model simplifies complexity; a bad model creates complexity.**

How to develop this sense? Study good code. nginx's `ngx_connection_t` knows nothing about HTTP. The Linux kernel's `struct sock` knows nothing about TCP. In good codebases, struct definitions themselves serve as domain model documentation. Seeing how they draw concept boundaries is more instructive than any design patterns book.

## One Takeaway

If you take away just one thing from this article, let it be this question: **why is this field in this struct?**

Field by field, ask yourself: does it describe the concept this struct represents? Or does it actually belong to something else and just happens to live here? If the answer is the latter, either a concept is missing or the boundaries are wrong.

Earlier articles had a checklist: look at structs, look at functions, look at dependency direction. Add one more: **look at field ownership.**

The data structures are sorted out, but loop still has `loop_on_read` — reading data, feeding the HTTP parser, checking completion status — behavior that belongs to fetch. Next article: making `js_fetch_t` truly take over the responsibilities it should own.

---

GitHub: https://github.com/hongzhidao/jsbench
