# There's an HTTP Parser Living Inside the Connection Layer

The last two articles polished the engine layer — `js_epoll_poll()` encapsulates event dispatch, `js_engine_t` combines epoll and timers. The engine layer is clean now.

But looking one layer up, I found something that didn't belong.

## What's Inside the Connection Struct

Here's `js_conn_t` — the core struct of jsbench's connection layer:

```c
typedef struct js_conn {
    js_event_t           socket;
    conn_state_t         state;
    SSL                 *ssl;

    const char          *req_data;
    size_t               req_len;
    size_t               req_sent;

    js_http_response_t   response;   /* ← what is this? */

    uint64_t             start_ns;
    int                  req_index;
    void                *udata;
} js_conn_t;
```

Socket, state, TLS, read/write buffer — all things a connection should have. But `js_http_response_t response`? That's an HTTP response parser, embedded directly in the connection struct.

There's an HTTP parser living inside the connection struct. This means: **this "connection" cannot exist independently of HTTP.**

Now look at `conn_do_read()` — the function that handles read events in the connection layer:

```c
static int conn_do_read(js_conn_t *c) {
    char buf[JS_READ_BUF_SIZE];

    for (;;) {
        ssize_t n;
        if (c->ssl) {
            n = js_tls_read(c->ssl, buf, sizeof(buf));
        } else {
            n = read(c->socket.fd, buf, sizeof(buf));
        }

        /* ... error handling ... */

        int ret = js_http_response_feed(&c->response, buf, (size_t)n);
        if (ret == 1) {
            c->state = CONN_DONE;    /* HTTP parse complete → connection state change */
            return 1;
        }
    }
}
```

Reading data is the connection layer's job. Parsing HTTP is the protocol layer's job. But in this function, both are mixed together with no boundary.

Then there's `js_conn_keepalive()`:

```c
bool js_conn_keepalive(const js_conn_t *c) {
    const char *conn_hdr = js_http_response_header(&c->response, "Connection");
    if (conn_hdr && strcasecmp(conn_hdr, "close") == 0)
        return false;
    return true;
}
```

To decide whether to reuse a connection, this function reads the HTTP `Connection` header. The connection layer depends on protocol-layer knowledge.

Three places, same problem: **conn and http have collapsed into one layer.**

## What This Means

jsbench has three layers from bottom to top:

```
  ┌─────────────────┐
  │      http        │  Protocol semantics: parse responses, check keep-alive
  └────────┬─────────┘
           │ depends on
  ┌────────▼─────────┐
  │      conn        │  Transport: connections, read/write, TLS
  └────────┬─────────┘
           │ depends on
  ┌────────▼─────────┐
  │     engine       │  Event-driven: epoll + timer
  └──────────────────┘
```

Ideally, each layer does its own job, with dependencies pointing downward. http uses conn to send data, conn uses engine for I/O. Upper layers depend on lower layers; lower layers don't know the upper layers exist.

But the actual code looks like this:

```
  ┌──────────────────────────────┐
  │       conn + http             │
  │                               │
  │  conn embeds http response    │
  │  conn reads HTTP headers      │
  │  conn calls http parser       │
  └──────────────┬────────────────┘
                 │
  ┌──────────────▼────────────────┐
  │           engine               │  ← this layer is clean
  └───────────────────────────────┘
```

The top two layers collapsed into one. Changing HTTP parsing logic means touching conn code. Changing conn's read strategy might break HTTP parsing. **The complexity of two layers isn't additive — it's multiplicative.**

Compare this with the engine layer. After the refactoring in articles 5 and 6, engine's dependencies are clean: `js_event_t` doesn't know what's above it, isolated through callbacks. When article 6 introduced `js_engine_t` and changed engine internals (from thread-local epfd to explicit engine parameter), the connection logic above was completely unaffected. **Changing a lower layer without affecting upper layers — that's the payoff of layering.**

The conn layer? If you wanted to switch HTTP parsing strategies, or make jsbench support a non-HTTP protocol, you'd find it nearly impossible — because conn and http code are entangled, and touching either side risks breaking the other.

**The core problem layering solves is dependency. Not eliminating dependencies, but making them unidirectional.** Once dependencies are unidirectional, each layer can be understood, modified, and evolved independently.

## Why AI Wrote It This Way

This is worth thinking about. AI's goal when writing code is "make it work." The most direct way to make an HTTP client work is: create connection, send request, read data, parse response, check keep-alive. String these steps together in a group of functions, and the functionality works.

But "make it work" and "make it evolvable" are two different goals. Layering serves the latter. **Layer boundaries aren't functional requirements — they're architectural decisions.** AI won't proactively say "we should draw a line here, separating transport from protocol" — because not drawing the line still works.

This echoes earlier experience. The engine refactoring in article 5 wasn't because AI couldn't modify epoll code — it's because AI wouldn't proactively say "epfd shouldn't be a parameter, it should be a property of the thread." Humans set the direction, AI executes well. The conn/http separation is the same: AI is fully capable of doing the mechanical split, but "whether to split, and where" — that's a human judgment call.

## Remove What's Unreasonable

Direction is clear: separate conn from http. Where to start?

There's a straightforward method for improving architecture: **find what's unreasonable and remove it.** `conn_do_read()` shouldn't do HTTP parsing — remove it. `js_conn_t` shouldn't embed an HTTP response — remove it. `js_conn_keepalive()` shouldn't read HTTP headers — remove it. Remove them one by one, and the layers naturally separate.

The reasoning is simple: **the more a function does, the more concepts it touches, and the more reasons it has to change.** This has a classic name — Single Responsibility. But I prefer the plain version: let each function do only the one thing it should do.

This isn't a rigid rule, though. `conn_do_write()` sets `c->state = CONN_READING` after finishing a write — transitioning from write to read is the connection state machine's natural flow, no need to force a split. **The criterion isn't "how many things does it do," but "will these things change independently?"** Reading bytes and parsing HTTP clearly will — you could easily swap the parsing strategy or the read strategy, each for its own reasons.

## Step 1: Introduce a Buffer to Separate Reading from Parsing

`conn_do_read()` needs to change — make it only read data, not parse. But where do the bytes go?

Currently it uses a stack-allocated temporary array, immediately feeding it to the parser. If we don't process immediately, the bytes need somewhere to live. Introduce `js_buf_t`:

```c
typedef struct {
    char   *data;
    size_t  len;     /* bytes of valid data */
    size_t  cap;     /* allocated capacity */
} js_buf_t;
```

With the buffer, `conn_do_read()` becomes:

```c
static int conn_do_read(js_conn_t *c) {
    js_buf_t *in = &c->in;

    for (;;) {
        js_buf_ensure(in, in->len + JS_READ_BUF_SIZE);

        ssize_t n;
        if (c->ssl) {
            n = js_tls_read(c->ssl, in->data + in->len, in->cap - in->len);
        } else {
            n = read(c->socket.fd, in->data + in->len, in->cap - in->len);
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            c->state = CONN_ERROR;
            return -1;
        }
        if (n == 0) return 1;  /* peer closed */

        in->len += (size_t)n;
    }
}
```

No `js_http_response_feed`, no `c->state = CONN_DONE`, no protocol-related logic whatsoever. **It does one thing: read bytes from socket into the buffer.**

HTTP parsing moved to the callers — worker and loop read from `c->in` after the read completes, feed the data to the HTTP parser themselves. Instead of conn pushing data to the parser, callers pull data from conn's buffer. **This is the most critical step in the entire layering work — conn no longer knows HTTP exists.**

Code change: [47e4d2c](https://github.com/hongzhidao/jsbench/commit/47e4d2c)

## Step 2: Give the Conn Layer a Proper Home

All connection-related code lived in `js_http_client.c` — the filename itself reveals the problem: how can a file called "HTTP client" be a clean transport layer?

Created `js_conn.h` and `js_conn.c`, extracting `js_conn_read()` as the first public interface. create, free, write, reuse, reset still remain in `js_http_client.c` — no rush to move everything at once.

I've been consciously avoiding over-design. Under-designed code is honest — it tells you "this part hasn't been fully thought through." Over-designed code is deceptive — those abstractions may guess wrong about the future, and when you actually need to change things, they're harder to modify than having no abstraction at all. **When you don't have enough information, doing less is safer than doing more.**

Code change: [5c8c3bc](https://github.com/hongzhidao/jsbench/commit/5c8c3bc)

## Step 3: Remove the Field That Doesn't Belong

Behavior is decoupled, the module exists. But `js_conn_t` still embeds `js_http_response_t response`. Decoupled in behavior, still bound in structure.

A good struct should be lean — **every field it owns should be something it needs, not something someone else needs.**

Where does the HTTP response go after removal? The answer is in existing mechanisms. `js_event_t` has a `void *data` field. Let callers own the response, associating it with the connection through `socket.data`:

```c
js_http_response_init(&responses[i]);
conns[i]->socket.data = &responses[i];
```

In read event handlers, retrieve the response from `socket.data`:

```c
static void worker_on_read(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    js_http_response_t *r = c->socket.data;
    /* ... */
}
```

`js_conn_keepalive()` moved into worker as a local function `worker_keepalive()`. The connection layer no longer needs to know anything about the HTTP protocol.

After removing `response`, `js_conn_t` becomes:

```c
typedef struct js_conn {
    js_event_t       socket;
    conn_state_t     state;
    SSL             *ssl;

    const char      *req_data;
    size_t           req_len;
    size_t           req_sent;

    js_buf_t         in;

    uint64_t         start_ns;
    int              req_index;
    void            *udata;
} js_conn_t;
```

No HTTP-related fields whatsoever. A connection is just a connection. **How lean a struct is directly reflects how well the layering is done.**

Code change: [0bbe6a3](https://github.com/hongzhidao/jsbench/commit/0bbe6a3)

## Step 4: Move the Type Where It Belongs

`js_conn_t` no longer depends on `js_http_response_t`, so it can finally move from `js_main.h` to `js_conn.h`. It couldn't move before because `js_http_response_t` was defined in `js_main.h` — embedding it meant being stuck there. Remove the dependency, remove the obstacle.

The conn layer is starting to take shape: types in `js_conn.h`, read implementation in `js_conn.c`, no dependencies on anything protocol-related. create, free, write still remain in `js_http_client.c`, but the direction is clear. No rush — one step at a time.

Code change: [edc2859](https://github.com/hongzhidao/jsbench/commit/edc2859)

## Looking Back

Four steps done. Back to the opening question: is that HTTP parser still living inside the connection layer?

It's gone. `js_conn_t` has no HTTP fields, `conn_do_read()` calls no HTTP functions, and the conn module has its own header and implementation files. conn doesn't know http exists — just as engine doesn't know conn exists.

Every step did the same thing: **find an unreasonable dependency and remove it.** No need to design a perfect layering scheme upfront, no need to move all the code at once. See something unreasonable, remove it, and the system is a little better than before. After a few steps, the layers naturally separate.

Layering isn't theory — it's a practical tool. Here are a few ways to check if your layering is working:

- **Look at structs.** Is every field something it needs, or is it holding something for someone else?
- **Look at functions.** Do they operate within a single layer? Reading bytes and parsing protocols aren't the same layer.
- **Look at dependency direction.** Does a lower layer reference types from or call functions in a higher layer?
- **Look at header files.** Can a module's `.h` compile without depending on unrelated types?

These checks don't require architecture diagrams. Just open the code.

---

GitHub: https://github.com/hongzhidao/jsbench
