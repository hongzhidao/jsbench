# AI Code Passed Every Test, but Every Request Failed

> Good architecture isn't designed upfront — it grows out of solving real problems.

Last time, I talked about code organization — file naming, prefix style, build separation. That was the skeleton. But once the skeleton is in place, the real test of architecture is whether it can support the evolution of functionality.

In this post, I'll use a concrete problem to drive architecture improvement: **fetch() doesn't support concurrency**.

## The Problem

One of jsbench's selling points is the standard `fetch()` API. Users can write `await fetch()` in JS, same as in a browser.

But I discovered a problem: `Promise.all` doesn't work.

```js
// In theory, 3 concurrent requests, total time ≈ 300ms
const responses = await Promise.all([
    fetch('http://localhost/delay/300'),
    fetch('http://localhost/delay/300'),
    fetch('http://localhost/delay/300'),
]);
// Actual time: ~900ms — sequential execution, no concurrency
```

I wrote a test to verify:

```js
// 3 requests, each with 300ms delay
// If concurrent: total ~300ms
// If sequential: total ~900ms

var t0 = Date.now();
var responses = await Promise.all(urls.map(url => fetch(url)));
var elapsed = Date.now() - t0;

// Result: elapsed ≈ 900ms
// CONCLUSION: fetch() is BLOCKING - no real concurrency
```

`Promise.all` with 3 fetches takes the same time as 3 sequential `await` calls. fetch is blocking. No real concurrency.

## Root Cause

One look at `js_fetch.c` and it's clear:

```c
static JSValue js_fetch(JSContext *ctx, ...) {
    // 1. Blocking DNS resolution
    int gai_err = getaddrinfo(url.host, url.port_str, &hints, &res);

    // 2. Create connection
    js_conn_t *conn = js_conn_create(...);

    // 3. Create a **local** epoll instance
    int epfd = js_epoll_create();

    // 4. Synchronous wait until response completes
    while (!done) {
        int n = epoll_wait(epfd, events, 4, 1000);
        // ... process events ...
    }

    // 5. Return an already-resolved Promise
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
    JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 1, &result);
    return promise;  // ← already resolved, not pending
}
```

Three things are wrong:

**DNS resolution blocks.** `getaddrinfo()` is synchronous — it blocks the entire JS runtime.

**Each fetch creates its own epoll.** Instead of registering with a global event loop, each call creates a local one, runs it to completion, and destroys it.

**The Promise resolves immediately.** By the time the function returns, the I/O is already done. The Promise is just a shell — wrapping an already-completed result.

The root cause: AI used a "fake async" approach — the function signature looks async (returns a Promise), but the implementation is entirely synchronous and blocking. The interface is Web Fetch API; the implementation is a synchronous HTTP client.

## I Know What the Right Architecture Looks Like

Having spent years on nginx, I know exactly how to solve this. The correct approach:

1. fetch() should create the connection, register it with a global event loop, and return a **pending** Promise
2. A global event loop manages all connections' I/O
3. When a connection's response arrives, resolve the corresponding Promise
4. DNS resolution also needs to be async

This is the nginx model — and the standard pattern for all high-performance async I/O frameworks: event-driven, non-blocking, centrally scheduled.

In fact, jsbench's C-path (the pure performance path) already uses this architecture. The worker thread's epoll instance manages hundreds of connections simultaneously, with a state machine driving each connection's lifecycle. There's even `js_loop.c` in the codebase, with data structures designed for managing pending fetches.

The JS-path's fetch() just isn't plugged into this system. AI took a shortcut — synchronous blocking, wrapped in a Promise. It works, but it's wrong.

## But Honestly, Doing It Manually Is No Small Task

I know how to fix it. But objectively speaking, if I were to do all of this by hand, the difficulty and workload are substantial:

- fetch() needs to change from synchronous blocking to async non-blocking, returning a pending Promise
- A global event loop needs to manage multiple concurrent connections
- Promise resolve/reject has to integrate with the event loop
- QuickJS's job queue needs to be driven correctly within the event loop
- DNS resolution must be made async to avoid blocking the event loop
- Error handling, timeouts, TLS handshakes all need to adapt to the new model

This isn't a few-line fix. It's an architecture-level change — turning fetch from a synchronous function into a truly asynchronous operation, touching the event loop, Promise machinery, and connection management end to end.

## Let AI Take the First Cut

So here's my approach: **explain the problem and direction clearly, and let AI do the fix**.

This might sound like cutting corners, but it's actually a legitimate form of architecture improvement in the AI era. In the last post, I said — get the architecture right, and AI becomes your entire team. The flip side: when you have a clear architectural direction, delegating the implementation to AI is perfectly reasonable.

The key is telling AI exactly these things:

1. **What the problem is**: fetch() is currently blocking; Promise.all can't achieve concurrency
2. **What the right direction is**: fetch should return a pending Promise; I/O should be driven by a global event loop
3. **What already exists**: C-path already has a complete event loop and connection management; js_loop.c has ready-made structures
4. **What the constraints are**: must integrate correctly with QuickJS's job queue; must maintain Web Fetch API semantics

You don't need to tell AI how to write every line. You give it the problem, direction, context, and constraints. It does the implementation.

This tells us something: **AI can improve architecture, as long as you explain the problem clearly.** Conversely, if you explain the architecture before writing code, AI can implement it quite well. But "explaining clearly" itself depends on experience — you need to know what's right before you can point AI in the right direction.

I believe AI's architecture ability will keep improving. In the first post, I said architecture is AI's most obvious weakness. But a weakness now doesn't mean a weakness forever. Like AI in chess — it wasn't as good as humans at first, then it surpassed everyone. Architecture is ultimately pattern recognition plus accumulated experience. There's no reason AI can't get good at it.

## Results

After AI made the changes, I ran the test:

```
Promise.all with 3 x 300ms delay:
  Total elapsed: 302ms          ← was 905ms
  RESULT: CONCURRENT (fetches ran in parallel)
```

905ms → 302ms. `Promise.all` is truly concurrent now.

The change touched 9 files. The core changes:

- **js_fetch.c**: Removed the blocking epoll loop. fetch() now registers the connection with a global event loop and returns a pending Promise
- **js_loop.c** (new): Centralized event loop — one epoll managing all connections, interleaving I/O events with QuickJS job queue execution
- **js_cli.c**: The original 34-line execution logic reduced to 5 lines, delegating to `js_loop_run()`
- **js_event_loop.c → js_epoll.c**: Renamed, since it was only epoll wrappers — not a real event loop

Looking at the results, AI did well. Once I explained the problem and direction clearly, it got the architecture right in one pass.

## How to Improve Architecture

Looking back at this change, the path is actually quite clear:

**1. Find the problem first.** The prerequisite for improving architecture is knowing where the problem is. There are generally two ways to find problems: reviewing the code, or writing test cases that cover it. This time I used both — I read `js_fetch.c` and spotted the synchronous blocking; I also wrote a concurrency test that quantified the problem as 905ms vs 300ms. No problem discovered, no starting point for improvement.

**2. Let AI take a crack at it.** Once you've found the problem, you don't have to fix it yourself right away. Describe the problem clearly, and AI can often fix it — and fixing a problem naturally brings architectural improvement along the way. This time AI turned fetch from synchronous blocking into an async event loop. That's not just a bug fix — it's the architecture moving one step in the right direction.

**3. Figure out the right direction.** Knowing the problem isn't enough — you need to know what "correct" looks like. This time it was a global event loop + pending Promises. That judgment came from experience. When experience is lacking, look at how mature projects do it — nginx, libuv, Node.js all use the same model.

**4. Explain clearly to AI, let it implement.** Problem, direction, existing foundations, constraints — explain these four things, and AI can produce a correct implementation. This time AI got it right in one pass: 9 files, 905ms → 302ms.

**5. Validate with tests.** Write the test before the change, run it after. 302ms was this round's acceptance criteria.

This path isn't specific to this one case. Any architecture improvement ultimately comes down to these steps: find the problem, think through the direction, implement, validate. The key differentiator is step two — "think through the direction" — which depends on experience, and is where humans still have an edge over AI.

Code changes: [1179d57](https://github.com/hongzhidao/jsbench/commit/1179d57)

## But the Story Doesn't End Here

AI finished the fix. `make test` — 14 tests, all green. Looks good.

But is it really?

fetch() changed — from synchronous blocking to an async event loop. What about bench async function mode? It also calls fetch(). Does it still work?

One look at `js_worker.c`:

```c
static void worker_js_path(js_worker_t *w) {
    JSRuntime *rt = js_vm_rt_create();
    JSContext *ctx = js_vm_ctx_create(rt);   // ← new context, no event loop

    while (!stop) {
        JSValue promise = JS_Call(ctx, default_export, ...);

        while (JS_ExecutePendingJob(rt, &pctx) > 0);

        w->stats.requests++;       // ← unconditionally +1
        w->stats.status_2xx++;     // ← unconditionally counted as success
    }
}
```

The worker creates a new JS context but never sets up an event loop. fetch() can't find a loop and throws an error. **Every single fetch fails.**

But the test reports:

```
Async function                       PASS (16576 reqs, 0 errors)
```

16,576 requests, 0 errors. **Every request actually failed, but the report says all succeeded.**

The program counted failures as successes — the worker never checks whether fetch actually got a response, just unconditionally records success. The test only looks at that number, so it can't catch the problem.

But this wasn't caused by our fetch change. **This code was broken from the start.** Before the change, fetch was synchronous blocking and did return responses. But the worker never checked the results, and the test never validated actual outcomes. It just happened to work before, masking the problem.

**AI wrote code that "runs" and AI wrote tests that "pass," but neither verifies what actually matters.** This is a classic trap with AI-generated code: the implementation and tests were written by the same AI, sharing the same blind spots. The implementation skips error checking, the tests skip error validation — they perfectly complement each other, creating an illusion that everything works.

How did I find this? By reviewing the code. I know that every worker thread should have its own event engine — an epoll_wait driving I/O. CLI mode has one. C-path has one. But bench function mode doesn't.

There are two ways to find problems: reviewing code and writing tests. The first problem in this article (fetch doesn't support concurrency) was caught by a test — 905ms vs 300ms, crystal clear. But this problem wasn't caught by tests, because the AI-written tests and AI-written implementation share the same blind spots: the implementation skips error checking, the tests skip error validation — they complement each other, creating an illusion that everything works. In cases like this, human review is the only way.

Once the problem was found, AI fixed it very fast. Create an event loop for the worker, use `js_loop_run()` to drive I/O, check the return value for success or failure — once I explained it, AI had it done in minutes. Bench async mode actually works now.

At this stage, AI introduces serious bugs — not occasionally, but regularly. And the bugs it introduces often aren't obvious crashes. They're like this one: the program runs, the tests pass, but the results are wrong. These are the most dangerous bugs, because you don't know they're there.

So code review remains the most effective quality assurance method. This isn't a new insight. I've worked on nginx unit, njs, and the nginx team — all of these projects have relatively few bugs, and beyond the skill of their developers, review is a mandatory part of the process. Every commit gets carefully reviewed — sometimes the reviewer spends more time than the person who wrote the code. This practice hasn't become obsolete in the AI era; if anything, it's more important now. AI writes code far faster than humans, and without review, bugs accumulate far faster too.

Of course, using another AI to review can also improve the situation. Two AIs working adversarially — one writing code, the other poking holes — is more reliable than one AI writing and testing itself. As AI gets better, I believe these kinds of issues will become much rarer for many projects.

But in the short term, human review is still necessary. And it depends on the nature of the project. Some projects prioritize speed — tools, scripts, things where you just need it to work and can fix issues as they come. But others are different, like server-side programs that must endure every kind of edge case: high concurrency, unexpected disconnections, memory leaks, timeout boundaries. These corner cases won't surface in happy-path tests — they need experienced eyes to catch.

So understanding architecture still matters. Not that you need to write every line yourself — but you need to be able to see what's wrong. AI can write code, fix bugs, improve architecture, but only if someone tells it where the problem is. If you can't see it yourself, AI won't tell you either.

Code changes: [5e6e438](https://github.com/hongzhidao/jsbench/commit/5e6e438)

## Takeaways

This article did two things: fixed fetch's concurrency problem, and along the way discovered and fixed a stats bug in bench async mode. Both changes pushed the architecture in the right direction.

Looking back, a few observations:

- Solving problems and adding requirements are both good opportunities to improve architecture
- AI can improve architecture, as long as you explain the problem clearly
- AI-written code and AI-written tests share the same blind spots — review remains the most effective quality assurance
- Understanding architecture isn't about writing code by hand, it's about seeing what's wrong

So far, we've been patching — fixing a problem here, improving a bit there. Next time, I want to discuss: what does good architecture actually look like?

---

GitHub: https://github.com/hongzhidao/jsbench
