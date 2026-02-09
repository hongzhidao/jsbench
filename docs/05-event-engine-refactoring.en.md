# AI Builds Fast, but Humans Draw the Blueprints

AI writing code is like constructing a building. The first few floors go up fast and solid — function naming, logic implementation, test coverage, all good. But as the building gets taller, problems show up. The upper floors don't depend on how fast you lay bricks — they depend on whether the blueprints are right. The blueprints are the architecture.

The event engine is one of jsbench's foundations. This post does one thing: **encapsulate the event engine's complexity so it doesn't spread to the rest of the system.**

The end result: `epoll_wait()` — the event engine's most critical system call — went from being scattered across multiple files to existing in exactly one place. Callers went from writing twenty lines of epoll boilerplate to writing one line: `js_epoll_poll(100)`. But this result wasn't planned from the start. We just felt the event engine wasn't clean enough, then improved it step by step. Each step revealed the next obstacle, until we looked back and realized — four refactors, and the complexity was fully encapsulated.

## Why the Event Engine Is Central

What jsbench does is straightforward: manage a large number of network connections simultaneously, send requests, receive responses. But "managing a large number of connections simultaneously" is itself a classic systems programming problem — the heart of the C10K problem.

The standard solution is event-driven I/O: instead of dedicating a thread to each connection, use a single event loop to monitor all connections for state changes, and process whichever connection has data ready. On Linux, the system call for this is epoll.

In jsbench, whether it's the C-path (pure performance path) with 65,536 concurrent connections or the JS-path's concurrent `fetch()` requests, epoll drives everything underneath. **The event engine is the software's core infrastructure.** If the infrastructure design is wrong, nothing built on top of it will be stable.

## The Old Design: It Worked, but Not Well Enough

The previous event dispatch looked like this:

```c
for (int i = 0; i < n; i++) {
    if (events[i].data.ptr == NULL) {
        /* timer */
        read(tfd, &expirations, sizeof(expirations));
        timer_expired = true;
        break;
    }

    js_conn_t *c = events[i].data.ptr;
    js_conn_handle_event(c, events[i].events);

    if (c->state == CONN_DONE) { /* ... */ }
    else if (c->state == CONN_ERROR) { /* ... */ }
}
```

This code works, but it has several problems.

**NULL is used to distinguish event types.** Timers register with a NULL pointer; connections register with a connection pointer. Dispatch checks for NULL first — NULL means timer, non-NULL means connection. This doesn't scale. If you add signals, pipes, or UDP later, every new event type means another if-else branch.

**Event types were defined but never used.** The code defined `JS_EVENT_CONN` and `JS_EVENT_TIMER` type tags, and `js_conn_t` embedded a `js_event_t` as its first field. But the actual dispatch never looked at those tags — it relied on the NULL check. Half-finished design.

**Processing logic was centralized.** All connection events funneled through `js_conn_handle_event()` — one function that switches on connection state, handles reads and writes, handles TLS handshakes. The dispatch loop had to know "this is a connection" to call it. The event engine and connection logic were coupled together.

To sum up: event type detection was scattered in the dispatch loop, epoll dependency was scattered across callers, connection processing logic was scattered in the event loop. **Complexity was spreading everywhere instead of being contained where it belongs.**

## The Core Idea: Let Events Know How to Handle Themselves

If I had to summarize this refactor in one sentence: **change from "dispatch logic checks the event type and calls the right handler" to "each event carries its own handler, and dispatch just calls it."**

If you're not familiar with C, think of it this way.

Imagine a front-desk receptionist (the event dispatch loop) answering incoming calls. The old approach: the receptionist checks the caller ID, looks up a table to figure out which department it's for, and transfers the call. When a new department is added, the receptionist's lookup table needs updating.

The new approach: each phone line is wired to a specific person when it's installed. When the phone rings, the receptionist doesn't look anything up — just transfers to whoever is wired to that line. Adding a new department means installing a new line and wiring it to the right person. The receptionist's workflow doesn't change at all.

In C, "wiring to the right person" means function pointers:

```c
struct js_event_s {
    int                   fd;       /* file descriptor */
    void                 *data;     /* custom data associated with event */
    js_event_handler_t    read;     /* called when readable */
    js_event_handler_t    write;    /* called when writable */
    js_event_handler_t    error;    /* called on error */
};
```

This is `js_event_t`. Every event registered with epoll is a `js_event_t`, carrying its own read, write, and error handlers. **The event knows how to handle itself.**

The architectural concept behind this is **abstraction**. A connection is an event. A timer is an event. In the future there might be signals, pipes. Their underlying implementations are completely different, but to the event engine, they're all the same thing — a `js_event_t` with an fd and handlers, nothing more. The event engine doesn't need to know "how a connection handles a read event" or "how a timer handles a timeout." It only needs to know "an event arrived, call the handler."

The value of abstraction is this: **it defines a boundary.** Above the boundary, all events look the same. Below it, they're each different. The event engine only deals with what's above the boundary, so it doesn't need to change as new event types are added. This is the fundamental reason a system can scale.

This pattern is a classic in systems programming. nginx's event model works exactly this way.

## What Actually Changed

The changes centered on "abstraction" — making `js_event_t` a truly unified event interface. When a connection is created, it binds its own read/write/error handlers. Timers do the same. To the event engine, there's no difference — they're all just a `js_event_t`.

**The dispatch loop became completely generic:**

```c
js_event_t *ev = events[i].data.ptr;

if (e & (EPOLLERR | EPOLLHUP)) {
    if (ev->error) ev->error(ev);
} else {
    if ((e & EPOLLOUT) && ev->write) ev->write(ev);
    if ((e & EPOLLIN) && ev->read)   ev->read(ev);
}
```

No NULL check, no type inspection, no concern about what ev actually is. Just call the handler. Any new event type added in the future won't require changing a single character here. That's the effect of abstraction — code above the boundary doesn't change when things below the boundary change.

Ran the tests. All passed. Committed.

The change touched 6 files, net +73 lines. AI did all of it. I only did three things: set the direction (introduce handler-based events), reviewed intermediate results, and corrected naming. No new abstraction layers were introduced — we just turned the existing `js_event_t` from an empty shell into something genuinely useful. This is the "just enough" from the fourth post.

Code changes: [d8d8f8f](https://github.com/hongzhidao/jsbench/commit/d8d8f8f)

## Continuing Improvements, AI Got Stuck

With handler-based dispatch done, I asked AI to keep improving the epoll usage. It couldn't figure out what to do.

The problem: every epoll operation (add, mod, del) required passing `epfd` — the epoll instance's file descriptor. This is a **dependency problem**: all epoll operations depend on the caller providing epfd, and the caller has to hold, pass, and manage that value itself.

If you're not familiar with programming, think of "dependency" this way: your apartment complex has a package locker, and every time you pick up a package you need a pickup code. If every delivery app requires you to manually enter the locker's ID number, you have to remember it and type it in every time. Get it wrong, and you can't get your package. That locker ID is an unnecessary dependency — the app could perfectly well know which locker belongs to your building.

Specifically, this dependency caused three problems.

**Callers had to manage epfd themselves.** Worker threads stored it in a local variable; the event loop stored it in a struct. Two different places, each managing their own copy — creation, passing, cleanup, all manual.

**The interface wasn't clean.** The event already knows its own fd (`ev->fd`) and its own handlers. But epoll operations still required passing an extra epfd — like having to announce which building you're in every time you make a phone call.

**Easy to get wrong when extending.** If someone added a new event type — say, a signal or a pipe — they'd have to remember to pass the right epfd. Pass the wrong one and the compiler won't complain; you'll only find out at runtime.

AI could see these weren't ideal, but didn't know which direction to take. It tried several approaches — storing epfd in `js_event_t`, using a global variable — none of them quite right.

## One Sentence Was Enough

The hint I gave it: **each thread has exactly one epoll.**

That single sentence, and AI immediately knew what to do.

Why was that sentence useful? Because it identified an architectural fact: in jsbench, each worker thread creates one epoll instance, and every event operation in that thread — adding connections, setting timers, changing listen states — goes to that same epoll. epfd isn't a "parameter"; it's the thread's infrastructure — one per thread, always.

Since it's a thread-level singleton resource, there's no need to pass it around. The dependency can be eliminated.

If you're not familiar with C, think of it this way. Imagine an office (a thread) with a whiteboard (epoll). Before, every time you wanted to write something on the whiteboard, you had to ask "where's the whiteboard?" But each office has exactly one whiteboard, and it's always in the same spot. You don't need to ask every time — walk into the office and just use it.

AI used C's thread-local storage (`__thread`) — each thread gets its own independent copy of `js_epfd`, completely isolated from other threads. With that, the epoll interface changed:

```c
// before: caller must pass epfd every time
js_epoll_add(epfd, &conn->socket, EPOLLIN | EPOLLOUT | EPOLLET);
js_epoll_mod(epfd, &c->socket, mask);
js_epoll_del(epfd, &c->socket);

// after: epfd is the thread's internal concern, callers don't deal with it
js_epoll_add(&conn->socket, EPOLLIN | EPOLLOUT | EPOLLET);
js_epoll_mod(&c->socket, mask);
js_epoll_del(&c->socket);
```

One fewer parameter, but the significance goes beyond saving a few keystrokes — **the epfd dependency was completely eliminated.**

**The interface semantics are clearer.** "Add this event to epoll" — the caller only needs to care about "which event, monitoring what," not "which epoll." Like mailing a package: you just write the destination address, you don't need to know where the post office is.

**Impossible to pass the wrong one.** Before, if someone accidentally passed the wrong epfd, events would register with the wrong epoll instance, and debugging that was painful. Now the error is eliminated at the source — you simply don't have the opportunity to get it wrong.

**Resource management is centralized.** epoll creation and cleanup are encapsulated in `js_epoll_create()` and `js_epoll_close()`. Thread starts, create. Thread ends, close. One in, one out, clean and simple.

This change wasn't fully complete — `epoll_wait()` still needed epfd, and callers still had to write their own event loops. But that's fine — **each refactor should be independent and valuable on its own.** This one's value was clear: eliminate the epfd dependency, simplify the epoll operation interface. The rest is for the next step.

Ran the tests. All passed. Committed.

Looking back, the amount of code changed was tiny — 4 lines of interface signatures in the header, a `__thread` variable and a close function in the implementation, and all epfd parameters removed from call sites. But behind those 4 lines was an architectural judgment: **the epoll instance is thread-level infrastructure, not a parameter to be passed around.** AI doesn't lack execution ability; what it lacks is this kind of judgment. It could tell the code wasn't elegant, but didn't know how to fix it — because "how to fix it" isn't a syntax question, it's an understanding of system architecture. But once you tell it the judgment, it immediately turns that judgment into a correct implementation. Sometimes AI doesn't need a lengthy prompt. It needs one precise judgment. Like mentoring a junior engineer — sometimes you don't need a page of documentation, just one sentence: "this thing shouldn't be a parameter, it should be a property of the thread." And they get it.

Code changes: [10e23aa](https://github.com/hongzhidao/jsbench/commit/10e23aa)

## The Next Step Wouldn't Budge — and It Wasn't epoll's Fault

With the epfd dependency eliminated, the natural next step was introducing `js_epoll_poll()` — wrapping `epoll_wait()` plus event dispatch into a single function.

What `js_epoll_poll()` needed to do was simple: call `epoll_wait()` to wait for events, then call each ready event's handler. Callers wouldn't need to write their own `epoll_wait` loop, iterate over the event array, or figure out whether it's a read or a write. One function call, done.

If you're not familiar with programming, think of it this way: earlier we solved the "where's the whiteboard" question (no more passing epfd). Now we want to go further — you don't even need to walk up to the whiteboard, check it, and notify the right people. An assistant does all that; you just wait for results.

This is exactly what the fourth post called "putting complexity in a box" — **expose only a simple interface, hide the internal complexity.**

But I found we couldn't do it.

Look at the event loop code in `js_worker.c`:

```c
for (int i = 0; i < n; i++) {
    js_event_t *ev = events[i].data.ptr;
    uint32_t e = events[i].events;

    /* Part 1: event dispatch — generic, epoll-related */
    if (e & (EPOLLERR | EPOLLHUP)) {
        if (ev->error) ev->error(ev);
    } else {
        if ((e & EPOLLOUT) && ev->write) ev->write(ev);
        if ((e & EPOLLIN) && ev->read)   ev->read(ev);
    }

    /* Part 2: connection post-processing — specific, conn-related */
    js_conn_t *c = (js_conn_t *)ev;

    if (c->state == CONN_DONE) {
        /* stats, keep-alive, reconnect... dozens of lines */
    } else if (c->state == CONN_ERROR) {
        /* reconnect... */
    } else {
        /* update epoll listen state... */
    }
}
```

The problem is obvious: **event dispatch and connection processing are tangled together.** Part 1 is generic epoll logic that could be encapsulated. Part 2 is connection-specific business logic — checking connection state, recording stats, handling reconnection — none of which belongs at the epoll level.

If you forced `js_epoll_poll()` to exist, it would either need to know what a "connection" is (breaking the abstraction), or it would need to return after dispatch and let the caller handle the rest (which defeats the purpose of encapsulation).

The event loop in `js_loop.c` had the same problem: right after dispatch, it casts `ev` to `js_conn_t *`, checks connection state, handles Promise resolve/reject. The epoll layer and connection layer were intertwined, impossible to separate.

This is coupling. Not an epoll problem — a conn problem. Connection processing logic was scattered in the event loop instead of being contained within the connection module itself.

AI tried, of course. It offered several approaches — adding a callback parameter to `js_epoll_poll()` so callers could pass in a "post-processing function," or having `js_epoll_poll()` return an event list for callers to iterate over.

These solutions could solve the immediate problem, but they were "problem-solving" thinking, not architecture thinking. There's a classic joke: I had a problem, I introduced a solution, now I have two problems. A callback parameter makes the epoll interface more complex. Returning an event list just moves the loop somewhere else. The problem isn't eliminated; it just changes form.

Architecture thinking is: **don't work around coupling — eliminate the coupling itself.** Connection processing logic doesn't belong in the event loop; it belongs in the connection module. Solve that root cause, and `js_epoll_poll()` writes itself cleanly.

## Refactor conn First

So the direction was: fix conn first.

Previously, the connection's read/write handler functions were static, with the signature `(js_event_t *ev)`, and the first thing they did was cast ev to conn. This meant connection processing was tied to the event system's type — you could only call them indirectly through `js_event_t *`.

The refactoring direction: **make connection processing functions take `js_conn_t *` directly, and make them public.** At the same time, `js_conn_create()` would no longer auto-bind handlers — handler binding would be the caller's responsibility.

The change looks small — function signatures changed, static became public, auto-binding was removed. But it broke a critical coupling: **connection processing logic no longer depended on the event system's type.** Callers could now call `js_conn_process_read()` in their handler, then do their own post-processing (stats, reconnection, resolve Promise). Connection concerns stay with connections; event concerns stay with events.

This was a prerequisite for introducing `js_epoll_poll()`. Once conn's processing logic was consolidated into handlers, `js_epoll_poll()` only needed to do epoll's own job — wait for events, call handlers — without knowing connections exist.

Ran the tests. All passed. Committed.

Code changes: [e8c5254](https://github.com/hongzhidao/jsbench/commit/e8c5254)

## Back to the Starting Point: js_epoll_poll()

Three preparatory steps done. Now we could introduce `js_epoll_poll()`. It does something simple: call `epoll_wait()` to wait for events, iterate over ready events, call each event's handler. Callers don't need to know how `epoll_wait` works, how to allocate the event array, or how to distinguish reads from writes and errors. One function, one parameter (timeout), done.

The effect was immediate:

```c
// before: 20 lines — allocate array, call epoll_wait, iterate, check read/write/error, call handler
struct epoll_event events[256];
while (!atomic_load(&w->stop) && active > 0) {
    int n = epoll_wait(epfd, events, 256, 100);
    // ... dozen lines of dispatch logic
}

// after: 3 lines
while (!atomic_load(&w->stop) && active > 0) {
    if (js_epoll_poll(100) < 0) break;
}
```

`js_loop.c` was the same — a dozen lines down to one.

Why was this possible? Because the previous three steps cleared each obstacle one by one:

1. **handler-based dispatch** — events carry their own handlers, dispatch logic doesn't need to know the event type
2. **thread-local epfd** — epoll operations don't need an fd parameter, `js_epoll_poll()` uses the thread-local variable internally
3. **conn decoupling** — connection post-processing lives in the handler, not in the event loop

Remove any one of these three, and `js_epoll_poll()` can't be written cleanly. Without handler-based dispatch, dispatch logic still needs to be written externally. Without thread-local epfd, the function signature still needs an epfd parameter. Without conn decoupling, connection state still needs to be processed in the loop body after dispatch.

This is why it wouldn't budge before. It's not that `js_epoll_poll()` itself is hard to write — it's only 20 lines. The hard part was making those 20 lines stand on their own, depending on nothing external.

One more change worth noting: `epoll_wait()` — the entire event engine's core call — went from being scattered across both `js_worker.c` and `js_loop.c` to being consolidated in `js_epoll.c` alone. Before, to understand how the event engine works, you'd have to search for `epoll_wait` across multiple files. Now there's only one place. The event engine's core logic lives in the event engine's own module. As it should be.

This is a good example for understanding what "hiding complexity" actually means — you don't need a technical background to follow.

The event dispatch logic — how to wait for events, how to tell reads from writes from errors, how to call the corresponding handler — all of this has real complexity. Previously, that complexity was exposed to every caller: `js_worker.c` wrote it once, `js_loop.c` wrote it again. If you ever needed to change the dispatch logic — say, add priority levels, change a buffer size, add an error handling strategy — you'd have to find every place that called `epoll_wait`, change them all, and keep them consistent. **Complexity was spreading.** The more places using it, the wider it spreads, the easier it is to miss something when making changes.

Now that complexity is packed into the `js_epoll_poll()` box. Callers see one line: `js_epoll_poll(100)`. How it waits, how it dispatches, how it handles errors — they don't know and don't need to know. If the dispatch logic needs changing in the future, you change one place. **Complexity is isolated. It doesn't spread.**

This is the fourth post's "putting complexity in a box — simple on the outside, clean on the inside." `js_epoll_poll()` exposes a single parameter (timeout) on the outside, and 20 lines of clear logic on the inside. The outside doesn't need to care about the inside, and changes inside don't affect the outside. This is architecture 101 — nothing fancy, no exotic design patterns. Just hide what should be hidden, expose what should be exposed.

Ran the tests. All passed. The entire change was a net -11 lines — same functionality, shorter code. Committed.

Code changes: [672c70c](https://github.com/hongzhidao/jsbench/commit/672c70c)

## AI and Refactoring

Four changes. Each one small, but the architecture moved forward step by step. AI did all the implementation. My role was always the same three things: set the direction, review intermediate results, correct details.

AI is remarkably efficient at refactoring. Specifically:

**Zero mistakes on mechanical changes.** Replacing `c->fd` with `c->socket.fd` everywhere — over a dozen occurrences scattered across different files. Humans easily miss some. AI did it in one pass, zero compiler warnings.

**Pattern transformations are accurate.** Tell it "replace type tag dispatch with handler-based dispatch," and it understands what the pattern means. It correctly splits the old switch-case into independent handler functions while preserving state machine semantics.

**Cross-file consistency is maintained.** After changing `js_event_t`'s definition, everything that uses it — epoll interfaces, connection creation, event dispatch, timer registration — needs to follow. AI maintains consistency across files. It won't change the interface and forget the call sites.

Refactoring is one of the most valuable use cases for AI programming today. Refactoring is characterized by: clear direction, well-defined patterns, broad scope of changes, highly mechanical work. This is exactly what AI excels at — large numbers of scattered but pattern-consistent modifications. Humans provide direction and judgment; AI handles execution and consistency.

A note on tooling. AI agents like Claude Code typically have two modes: fully automatic (AI makes decisions and executes autonomously) and manual confirmation (each step requires human approval). For refactoring, I recommend manual confirmation mode. The reason is simple: refactoring involves architectural judgment, and whether each step should be taken — and how far — needs a human call. Fully automatic mode works for purely mechanical tasks where the direction is completely decided, but refactoring often requires mid-course adjustments — like in this post, where we set out to improve epoll but discovered we needed to fix conn first. Hand that judgment to AI's autonomous decision-making, and it'll likely plow ahead in the wrong direction.

Another point worth noting: refactoring is precisely when **over-design** and **under-design** are most likely to happen. Change too little, and the coupling remains, blocking future refactors. Change too much, and you introduce abstraction layers that aren't needed yet, adding complexity instead of reducing it. In this case, decoupling conn — if changing the function signatures was enough, don't simultaneously build a full conn lifecycle management framework. But if you don't finish the changes that matter, you'll have to come back and patch things when introducing `js_epoll_poll()`. There's no formula for getting this balance right. It comes down to your own judgment about the code.

Speaking of refactoring — the concept deserves a few extra words. If you're not yet familiar with refactoring, it's worth studying specifically. Martin Fowler's *Refactoring* is the foundational text, but I'd recommend Joshua Kerievsky's *Refactoring to Patterns* even more — it's more readable, combining refactoring techniques with design patterns, and very practical. The core idea of refactoring is: **improve the internal structure of code without changing its external behavior.** Functionality stays the same, tests keep passing, but the code becomes clearer and easier to extend.

The role of tests in refactoring is clear-cut: they don't tell you how to change, but they tell you whether you broke anything after changing. We run tests after every refactor — that's basic discipline. **Change the structure, preserve the behavior, verify with tests** — that's the refactoring workflow.

Earlier posts said architecture is AI's weak spot. But refactoring — adjusting code along a human-given architectural direction — AI does very well. "Figuring out what to change" is the human's job. "Correctly applying the change to every line of code" is AI's job. This division of labor is natural. So my advice is: **learn refactoring, then let AI do it.** You identify the code smells, judge which direction to take; AI applies the changes to every line. This division of labor is extremely efficient.

## Takeaways

Looking back at these four refactors, they were really doing the same thing: **encapsulating the event engine's complexity, layer by layer.**

Step one: define boundaries through abstraction — above the boundary, all events are the same; below it, each is different. The event engine only deals with what's above, so it doesn't change as event types are added. Step two: eliminate dependencies — if something is infrastructure, don't make everyone carry it around. Step three: break coupling — don't work around the problem; put each module's logic back where it belongs. Step four: hide complexity — pack implementation details into a box so they don't spread across the system.

In the end, `epoll_wait()` appears in exactly one place. The event engine's complexity is fully encapsulated.

These four moves — **define boundaries, eliminate dependencies, break coupling, hide complexity** — aren't epoll-specific techniques. They're fundamentals for all architectural work. Whether you write C or JavaScript, build backends or frontends, when code feels like it "won't budge," it's usually because one of these four areas needs attention.

And this process wasn't visible from the start. We didn't draw a four-step roadmap and execute it in order — each step revealed the next problem only after the previous one was solved. We wanted to encapsulate epoll_wait, and discovered epfd was a dependency. We eliminated epfd, and discovered conn was coupled into the event loop. We broke the coupling, and js_epoll_poll() fell into place naturally. **Good architecture isn't designed upfront — it grows out of solving concrete problems one at a time.** But the prerequisite is having a sense of direction — knowing where complexity is, and knowing where it shouldn't be.

Saying AI has an architecture weakness isn't entirely accurate. The more I use it, the more I find AI actually possesses this architectural knowledge — abstraction, decoupling, encapsulation, it knows all of it. It's just that in practice, it tends to defer the choice to you: it lists several options but won't commit to a direction on your behalf. This might be related to how it's trained — I'm not an expert on that, just observing from usage.

But AI is genuinely a powerful assistant. Its execution ability is already very strong. If it becomes more proactive in architectural judgment in the future, collaboration efficiency will jump another level. And even when that day comes, understanding architecture will still be valuable — because you need to judge whether the direction AI gives you is right. **One architectural judgment from you, and AI can apply it to every line of code across dozens of files. The more accurate the judgment, the bigger the leverage.**

The event engine encapsulation wraps up here. The architectural improvements continue — next post is about redesigning the timer, using Igor Sysoev's red-black tree implementation, which I consider the best version out there.

---

GitHub: https://github.com/hongzhidao/jsbench
