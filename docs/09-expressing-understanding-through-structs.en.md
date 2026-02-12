# Expressing Understanding Through Structs

The code runs, tests pass, features work — but something feels off. This feeling has come up repeatedly in earlier articles: an HTTP parser embedded in the connection layer, two request types doing the same job, borrowed pointers with unclear lifetimes. Every time I dug deeper, the root cause was the same — **a concept that should exist in the code, but doesn't.**

This article is about how to find those missing concepts.

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

## Structs Are Expressions of Domain Understanding

Looking back at both changes — introducing `js_http_peer_t` and introducing `js_fetch_t` — they do the same thing: **recognizing a domain concept that the code didn't express, and giving it a struct.**

DDD (Domain-Driven Design) says: code should reflect your understanding of the problem domain. In C, this principle is especially direct — your understanding of the system ultimately manifests as `struct` definitions.

`start_ns` and `response` were scattered across two places because there was no concept of "HTTP peer." Once `js_http_peer_t` exists, they naturally fall into place. conn, timer, and response were mixed into loop's struct because there was no concept of "a fetch operation." Once `js_fetch_t` exists, they naturally fall into place. engine was stored in both loop and worker because there was no concept of "thread context." Once `js_thread_t` exists, it naturally falls into place.

**Structs aren't storage containers — they're expressions of domain concepts.** Which struct a field belongs to reflects your understanding of "who owns what." When the understanding is right, fields are in the right place. When the understanding is wrong or missing, fields scatter to where they don't belong — exactly the problems we've been seeing.

This also explains why AI doesn't tend to make these improvements proactively. AI writes code with the goal of "making it work," and it puts data wherever is most convenient — wherever it's needed. But "convenient" and "correct" aren't the same thing. Putting `start_ns` in conn is convenient, but not correct. Putting fetch allocation in loop is convenient, but not correct. **Identifying domain concepts, judging ownership, making structural decisions — that's human work.**

## Where the Difficulty Lies

It's easy to say — find the right model. It's hard to do.

The difficulty is that "right" isn't obvious. `js_http_peer_t` looks simple in hindsight — response plus start_ns, of course they belong together. But before it existed, nobody felt that `start_ns` in conn was wrong. The code ran, tests passed, features worked. **The problem isn't "it's broken" — it's "not realizing it could be better."**

Even harder is identifying the truly core models. `js_fetch_t` isn't just moving fields around — it redefines the boundary between fetch and loop, changes allocation strategy, reassigns timeout handling, reshapes function signatures. Introducing one struct triggers a cascade of changes. **A good model simplifies complexity; a bad model creates complexity.** The difference comes down to whether you truly understand the concept boundaries in your problem domain.

There's no shortcut to developing this ability, but there are methods.

**Practice.** Not just writing more code — but looking back at what you wrote. Is this struct really right? Do these fields really belong here? Every article in this series has been re-examining existing code. Refactoring isn't extra work done after the fact — it's the natural result of deepening understanding.

**Study good work.** nginx's `ngx_connection_t` knows nothing about HTTP. The Linux kernel's `struct sock` knows nothing about TCP. In good codebases, struct definitions themselves serve as domain model documentation. Seeing how they draw concept boundaries is more instructive than any design patterns book.

**Think more.** While writing code, pause and ask yourself: what is this thing? What does it own? Who does it belong to? These questions sound simple, but answering them seriously often reveals structural problems. Every change in this article started from such a simple question.

The DDD literature is vast and full of concepts. But in C, the core fits in one sentence: **figure out what concepts exist in your problem domain, what each concept should own, and express it with a struct.**

## One Takeaway

If you take away just one thing from this article, let it be this question: **why is this field in this struct?**

Next time you write or read code, look at a struct and ask yourself, field by field: does it describe the concept this struct represents? Or does it actually belong to something else and just happens to live here?

If a struct has fields that don't belong to it, either a concept is missing — a new struct is needed — or the concept boundaries are wrong — they need to be redrawn.

Earlier articles had a checklist: look at structs, look at functions, look at dependency direction. This article adds one more: **look at field ownership.** Every field should belong to the concept it lives in, no more, no less. Get that right, and module boundaries, function responsibilities, and code organization tend to fall into place on their own.

The data structures are sorted out, but loop still has `loop_on_read` — reading data, feeding the HTTP parser, checking completion status — behavior that belongs to fetch. The next article enters the deep waters of JS: making `js_fetch_t` truly take over the responsibilities it should own.

---

GitHub: https://github.com/hongzhidao/jsbench
