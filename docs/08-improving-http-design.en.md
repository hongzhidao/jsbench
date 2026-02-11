# Improving the HTTP Design

The previous articles cleaned up three layers from the bottom up. engine has `js_engine.h`, conn has `js_conn.h`, buffer has `js_buf.h`, timer has `js_timer.h` — each module with its own header, clean boundaries, unidirectional dependencies.

But what about HTTP?

## HTTP Is Scattered Everywhere

Look at where HTTP-related code lives:

- **Request type definitions** — in `js_main.h`, crammed together with dozens of other types
- **Request serialization** — in `js_util.c`, mixed with URL parsing, time formatting, and other utilities
- **Response type definitions** — also in `js_main.h`
- **Response parsing** — in `js_http_parser.c`
- **Connection creation and management** — in `js_http_client.c` (but the last article already stripped HTTP from conn, so this filename is wrong)
- **Keep-alive logic** — in `js_worker.c`
- **Response data feeding** — duplicated in both `js_worker.c` and `js_loop.c`

The engine layer has a home. The conn layer has a home. **HTTP has no home.** Its code is scattered across seven or eight files, with no unified header file and no clear module boundary.

Compare:

```
engine layer:  js_engine.h + js_engine.c + js_epoll.h + js_epoll.c   ← clean
conn layer:    js_conn.h + js_conn.c                                  ← clean
HTTP layer:    scattered across js_main.h, js_util.c, js_http_parser.c,  ← messy
               js_http_client.c, js_worker.c, js_loop.c, js_fetch.c
```

This can't be cleaned up in one pass. This article starts with the request.

## The Request: Two Types Doing One Job

jsbench sends requests in two steps: first extract request information from JS scripts (URL, method, headers, body), then serialize it into HTTP message bytes.

AI defined two structs for these two steps:

```
js_request_desc_t   → the intent: url(string), method, headers, body
js_raw_request_t    → serialized result: data(message bytes), len, url(parsed)
```

Two steps, two types. It works, but there are problems.

**The URL is stored twice.** desc has the URL string, raw has the parsed URL struct. Both must be passed during serialization — the same request, split into two parameters.

**The serialized result carries responsibilities it shouldn't have.** `js_raw_request_t` should just be message bytes, but it also stores a parsed URL for subsequent DNS resolution and connection creation. Something called "raw request" is carrying address information.

**Ownership is unclear.** Some fields in the request descriptor are heap-allocated, some are literals. Cleanup relies on comments reminding you what to free:

```c
/* method: only free if we strdup'd it */
/* This is a simplification -- in production we'd track ownership */
```

When you need comments to explain whether memory should be freed, the ownership model has a problem.

Three issues, one root cause: **the same job was split into two types.**

## Merging

The direction is simple: merge two types into one.

```
js_request_t   → url(parsed), method, headers, body
```

The key change: **url goes from a string to a parsed struct, directly part of the request.** No separate URL passing, no storing a copy in the serialization result.

The serialization function goes from four parameters to three — all request information is in one object, the result writes to `js_buf_t` (the buffer abstraction introduced earlier gets naturally reused). Cleanup uses a single `js_request_free()`, all fields uniformly allocated and freed, no comments needed.

`js_raw_request_t` disappears. What config stores changes from "message bytes + URL" to a pure `js_buf_t` byte array. The URL is promoted to the config level — all requests share the same target address, one copy is enough. Workers no longer need indirect paths like `config->requests[0].url.host`, just `config->url.host` directly.

Seven files, net reduction of 21 lines. Behavior unchanged.

Code change: [ae6d011](https://github.com/hongzhidao/jsbench/commit/ae6d011)

## Connection Output: From Borrowing to Owning

With the request unified, look at the connection layer next. `js_conn_t` sends data using three fields:

```c
const char *req_data;   /* points to external data */
size_t      req_len;
size_t      req_sent;
```

The connection doesn't own this data — it just borrows a pointer. On the worker path, this is fine — data lives in config, whose lifetime outlasts the connection. But on the fetch path, things get messy: the serialized message is heap-allocated, the connection borrows the pointer, and `js_loop.c` has to separately store a `raw_data` copy to handle freeing.

The same block of memory, two places worrying about its lifetime.

The fix is straightforward: **let the connection own its output buffer.**

```c
js_buf_t out;   /* connection-owned output buffer */
js_buf_t in;    /* connection-owned input buffer */
```

`js_buf_t` gets a `pos` field to track how much has been sent, replacing the original three fields. `js_conn_set_request()` becomes `js_conn_set_output()` — the connection doesn't need to know whether it's sending HTTP or anything else. This is the same direction as the last article's removal of the response from conn: **the conn layer shouldn't know about HTTP.**

The fetch path becomes clean: serialize, copy into conn's buffer, free the original immediately. All three `free(p->raw_data)` calls in `js_loop.c` disappear.

Code change: [7cb01d6](https://github.com/hongzhidao/jsbench/commit/7cb01d6)

## The Connection Layer Comes Home

The last article ended with: create, free, write are still in `js_http_client.c`, the direction is clear, no rush.

Now it's time. After the output buffer replaced `req_data`, every function in `js_http_client.c` is a pure connection operation — create, free, set_output, process_write, reuse, reset. Not one has anything to do with HTTP. A file called "HTTP client" with no HTTP in it.

Move everything into `js_conn.c`. During the merge, I found both files had their own copy of `conn_try_handshake()` — one for the read path, one for the write path. Unified into one, with callers deciding what to do after handshake completes.

`js_http_client.c` deleted. Looking back at the opening table:

```
conn layer:  js_conn.h + js_conn.c   ← complete
```

The last article gave conn a home. This article moved the whole family in.

Code change: [8492906](https://github.com/hongzhidao/jsbench/commit/8492906)

## Looking Back

Three changes, two threads.

The first: **clarifying ownership.** Two request types become one, eliminating data duplication and freeing ambiguity. Adding an output buffer to conn, eliminating borrowed pointer lifetime issues. In C there's no borrow checker — ownership is tracked by the programmer. The simpler the model, the fewer the mistakes.

The second: **letting code come home.** Requests now have a unified type `js_request_t`. All connection operations are back in `js_conn.c`. Code from one module scattered across multiple files is like scattered tools — not unusable, but you have to look for them every time.

**Splitting and merging are the same judgment.** The last article split (conn and HTTP); this article merges (request and URL, conn's implementation files). The standard is the same: **things that change independently go apart; things that change together go together.** Connection I/O and HTTP parsing each have their own reasons to change — split. Request and URL always change together — merge. create, free, write are all connection operations — merge into one file.

**Good infrastructure gets reused naturally.** `js_buf_t` was introduced for conn's read buffer. In this article it first stores serialization results, then serves as conn's output buffer, needing just one added `pos` field. **Infrastructure's value isn't in how cleverly it's designed, but in being simple enough to reuse without explanation.**

Requests and connections are sorted out. But looking back at the opening list — plenty of scattered HTTP problems remain.

## Where Does the Next Step Go

I originally wanted to solve all the HTTP problems in this one article. But after these three changes, I hit a familiar situation — some things can't be moved yet, and forcing them would only make the code messier.

It's the same pattern as when encapsulating epoll: wanting to push straight toward the goal, only to find a pile of coupling problems blocking the way. So these three changes — request unification, output buffer, connection homecoming — were actually detours. Not because I didn't want to go straight to the point, but because the road wasn't ready yet.

But the entry point is clear.

## Who Uses conn

conn is the transport layer. To improve HTTP's design, I first need to understand who uses conn and how.

After going through the source code, two places use it.

**The first: the worker C path** (`js_worker.c`)

The worker creates connections, sets up read/write callbacks. In the callbacks, it reads data, feeds it to the HTTP parser, determines completion or error. After a request completes, it records stats, checks keepalive, and decides whether to reuse or reconnect.

This path is straightforward. **The worker sends requests, receives responses, manages connections.** Clear responsibilities, no extra layers.

**The second: the JS fetch path** (`js_fetch.c` + `js_loop.c`)

fetch parses the URL, resolves DNS, creates the connection, serializes the request. Then it calls `js_loop_add()`, handing the connection to loop.

From that moment on, fetch is done.

loop sets up read/write callbacks. In the callbacks, it reads data, feeds the HTTP parser, determines completion or error. On success it resolves the promise; on failure it rejects.

Wait.

fetch created the connection but doesn't manage it. Reading, writing, parsing, completion checks, error handling — **it's all in loop.** loop is an event loop, supposedly generic infrastructure. But it now knows about `js_http_response_feed`, about `HTTP_PARSE_BODY_IDENTITY`, about whether a peer close means completion or error.

Compare: engine is also event-processing infrastructure. It knows nothing about conn, nothing about HTTP. **That's what clean looks like.**

loop shouldn't be handling HTTP. **fetch should.**

## This Is as Far as This Article Goes

The direction is clear, but the change is substantial. `loop_on_read` and `worker_on_read` are nearly identical — reading data, feeding the parser, state transitions. To let fetch take over HTTP responsibilities, I'd first need to extract this duplicated logic, then redraw the boundaries. That's not a few-line change. It goes in the next article.

Looking back at this article's three changes — request merging, output buffer, connection homecoming — none came from an architecture diagram. They came from reading source code line by line. Seeing two structs doing the same job led to merging. Seeing borrowed pointer lifetime issues led to the buffer. Seeing a filename that didn't match its contents led to the homecoming. Now discovering that loop shouldn't handle HTTP — that also came from reading the code and feeling something was off, then digging deeper to understand why.

**There's no shortcut to reading source code well.** Read good code, read your own code, and gradually develop a sensitivity for when something feels wrong. Architecture is sometimes a kind of aesthetic — you can't quite explain why something feels uncomfortable, but your instinct tells you there's a problem. Then you find the reason, fix it, and it feels right.

---

GitHub: https://github.com/hongzhidao/jsbench
