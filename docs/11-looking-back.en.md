# Pausing to See How Far We've Come

Ten technical articles, thirty-some commits. Time to stop and look back.

## What We Did

In hindsight, it was one thing: **putting complexity where it belongs, one layer at a time.**

First, the skeleton (article two) — naming conventions, file prefixes, build separation. Seems trivial, but it defines a project's identity.

Then fixing problems (article three) — fetch didn't support concurrency, bench counted failures as successes. Fixing problems naturally drove architecture improvements: synchronous blocking became an async event loop.

Then came bottom-up refactoring:

- **Event engine** (article five): four steps of encapsulation, `epoll_wait()` went from scattered across two files to existing in exactly one place
- **Work engine** (article six): red-black tree timers + epoll assembled into `js_engine_t`, one per thread
- **Connection layer** (article seven): removed HTTP from conn, introduced buffers, made conn protocol-agnostic
- **HTTP layer** (article eight): unified request types, gave conn its own output buffer, moved connection operations home
- **Structs** (article nine): `js_http_peer_t`, `js_fetch_t`, `js_thread_t` — expressing domain concepts through structs
- **Fetch encapsulation** (article ten): split files, moved behavior, severed dependencies — loop became an 85-line pure scheduler

Two reflective pieces were interspersed. Article four: good architecture is just enough. Article one: AI builds fast, but humans draw the blueprints.

## Architecture Comparison

All those words — let's just look.

**AI's first version:**

```
               jsb.h (318 lines, all declarations)
                      │
   ┌──────────────────┼──────────────────┐
   │                  │                  │
fetch.c          worker.c         http_client.c
544 lines         239 lines          227 lines
synchronous       event dispatch      conn + HTTP
+ Headers         + conn handling     mixed together
+ Response        + HTTP parsing
+ fetch()         + stats

              event_loop.c
           45 lines, thin epoll wrapper
```

One header file for all declarations. fetch is synchronous blocking with three classes crammed into one file. worker simultaneously handles event dispatch, connection processing, HTTP parsing, and statistics. conn and HTTP are fused together. event_loop is just a thin epoll wrapper, not a real event engine.

**Every file knows about every other file. Changing one place can affect everything.**

**Now:**

```
┌─────────────────────────────────────────────────────┐
│  Application layer                                   │
│  js_worker.c    js_fetch.c    js_loop.c    js_cli.c  │
│  (bench C-path)  (JS fetch)    (scheduler)  (CLI)    │
├─────────────────────────────────────────────────────┤
│  HTTP layer                                          │
│  js_http.h    js_http_parser.c                       │
│  js_headers.c    js_response.c                       │
├─────────────────────────────────────────────────────┤
│  Connection layer                                    │
│  js_conn.h + js_conn.c    js_buf.h                   │
├─────────────────────────────────────────────────────┤
│  Engine layer                                        │
│  js_engine.h + js_engine.c                           │
│  js_epoll.h + js_epoll.c    js_timer.h + js_timer.c  │
├─────────────────────────────────────────────────────┤
│  Infrastructure                                      │
│  js_unix.h    js_clang.h    js_time.h    js_rbtree.h │
└─────────────────────────────────────────────────────┘

Dependency direction: ↓ downward only, never up
```

32 files, 5 layers. Each layer depends only on layers below it, unaware of layers above. Engine doesn't know about connections, connections don't know about HTTP, loop doesn't know about fetch. **Changing any layer doesn't affect the others.**

From 13 files, 1 header, no layering — to 32 files, 12 headers, 5-layer architecture. Total code is similar — from 2800 lines to 3900, with most of the increase being infrastructure (red-black tree, timers, buffers). But each file is smaller, and each module does one thing.

## A Few Reflections

**Architecture doesn't need to be planned all at once.** We never drew a complete architecture diagram and executed it step by step. Each time we did one thing — found a problem, fixed it. After fixing it, the next problem appeared. Ten rounds later, the architecture grew into what it is now. Clear in hindsight, visible only one step at a time.

**The toolkit is smaller than you'd think.** Count the architecture techniques across ten articles: define boundaries, eliminate dependencies, decouple, hide complexity, aggregate related things, express concepts through structs. Six techniques, used repeatedly. Good architecture isn't about knowing many tricks — it's about applying a few in the right places.

**AI is an excellent executor.** Every refactoring: humans set the direction, AI carries it out. Zero errors on mechanical changes, cross-file consistency maintained, refactoring efficiency is remarkably high. Humans provide judgment, AI translates it to code — this division of labor is already mature.

**Refactoring and writing are the same thing.** Writing articles frequently revealed code problems — when trying to explain why code is written a certain way, if the explanation doesn't hold up, the code has a problem. Writing forces you to turn vague intuition into clear expression, and that process itself deepens your understanding of the system.

## What's Next

The broad architectural framework is now stable. What comes next is mostly detail work — interface naming, error handling, edge case coverage, performance optimization. Less dramatic, but equally important. What takes a project from "it works" to "it works well" is exactly this kind of detail.

jsbench will continue to be maintained and developed. The goal is to make it a genuinely useful benchmarking tool — as fast as wrk, but with standard JS for writing test scripts, ready to use out of the box.

If you're interested in C systems programming, event-driven architecture, or AI-assisted programming, you're welcome to join in:

- Issues, PRs, design discussions — all welcome
- The codebase is small (under 4000 lines of C), well-suited for learning systems programming
- The architecture articles and code are synchronized — you can read them side by side

Thanks for reading this far.

---

GitHub: https://github.com/hongzhidao/jsbench
