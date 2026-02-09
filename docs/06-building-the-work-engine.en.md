# Nothing Changed, but the Engine Is Built

The last post encapsulated the event engine into a single line: `js_epoll_poll(100)`. This post tackles another piece of infrastructure: timers.

But that 100 — the timeout parameter to `epoll_wait()` — is exactly the problem. It's hardcoded. Whether the nearest timer fires in 5 milliseconds or 5 seconds, the event loop always waits a fixed 100 milliseconds before checking. The timer says "wake me in 5ms," and the event loop replies "I'm not free for another 100ms."

Fixing this requires redesigning the timer system. And before redesigning timers, we need to lay the foundation.

This post only lays the foundation — introducing a red-black tree, time utility functions, and a standalone timer module. No existing timer logic has been changed yet. It sounds like "nothing got done," but this is the most important step.

## Current Timers: Three Ways to Do One Thing

Let's start with the problem.

jsbench has three completely different timer mechanisms. The C-path uses an OS-provided timer file descriptor — when time's up, epoll receives an event. The JS-path manually checks the current time at the start of each loop iteration, compares it against the deadline, and exits if it's past. Request timeouts live in the event loop — each iteration walks all pending requests and checks them one by one.

Three approaches doing the same thing: "wait a while, then do something." But their implementations are entirely different, and none knows the others exist.

To unify them, we need a generic timer module. But a timer module can't be written in a vacuum — it needs data structures, time types, and language utilities as infrastructure. **You can't build a house on sand.**

## Why Lay the Foundation First

If you're not familiar with programming, think of it this way.

Say you want to renovate your kitchen. The end goal is a kitchen you can cook in. But you can't buy a stove on day one — you need to make sure the plumbing is connected, the wiring is in place, the walls are level. Nobody sees this work, and when it's done you don't feel it's there. But without it, the stove won't install, or if it does, it won't work.

That's what infrastructure is: **when it's there, you don't notice it. When it's missing, nothing works.**

More importantly, infrastructure determines what the layers above it can do and how far they can go. If the plumbing only has cold water, you can't install a hot water faucet. If time types aren't properly abstracted, the timer module ends up with raw system calls everywhere — no better than what we have now. **The quality of the foundation determines the ceiling of the house.**

Many people at this point would rush to write features — "unify the three timers first, fill in the infrastructure as we go." That approach isn't impossible, but it usually goes like this: halfway through the feature you discover too many missing pieces, and when you go back to add them, the feature you already wrote doesn't fit the new infrastructure, so you end up going back and forth. **Laying the foundation first looks slower, but the total distance is shortest.**

So this change does one thing: lay the foundation. No rush to unify the three timers — that's the next step.

## Choosing the Data Structure: Red-Black Tree

A timer needs three operations: add ("wake me in x milliseconds"), delete ("never mind, the request already completed"), and find the nearest ("when does the next alarm go off?").

This requires a data structure.

Imagine you're managing a pile of to-do items, each with a deadline. You need to know which one is most urgent at any time, add new ones quickly, and cross off completed ones quickly.

**Simplest approach: a random pile of papers.** Find the most urgent one? Flip through the entire pile. That's what the current request timeout does — every loop iteration walks all pending requests.

**Red-black tree: a self-sorting filing cabinet.** Items go in sorted automatically. The most urgent is always at the front. Adding, removing, checking the front — every operation is fast. 100 items: at most 7 comparisons. 1,000 items: at most 10. 65,536 items: at most 16.

There's another common choice called a min-heap — checking the front takes just 1 step, and adding is fast too. But there's a catch: if you need to cross off something that's not at the front (say a request completed early and you want to cancel its timeout), a min-heap has to search through the pile to find it. Red-black trees don't have this problem — you're holding a reference to that item, just remove it directly.

**The red-black tree has no weak spot across all three operations.** For timer management, it's the most balanced choice.

This is a classic selection judgment. AI would suggest a min-heap — in most textbooks, timer management does use min-heaps. But textbook scenarios typically don't require frequent deletion of arbitrary nodes. jsbench does — when a request completes, its timeout timer needs to be canceled; when a connection closes, its idle timer needs to be canceled. **Selection isn't about choosing the most common option — it's about choosing what fits your scenario best.** That requires a clear understanding of your own requirements.

## Whose Red-Black Tree

Data structure decided. Next question: write our own or use an existing one?

Red-black tree implementations aren't simple — insertion and deletion involve rotations and recoloring, with error-prone details. But this isn't something you need to write yourself. Igor Sysoev implemented one in 2002 when writing nginx. Over the past 20-plus years, that tree has been battle-tested on hundreds of millions of connections worldwide. He later wrote an improved version for njs (nginx's JavaScript engine) — more flexible, more generic. In my view, it's the best red-black tree implementation there is.

The tree's most important design feature is **intrusive nodes** — the tree node isn't a separately allocated object; it's embedded directly into the user's struct. If you're not familiar with programming, think of it this way: suppose you want to sort a bunch of folders by date. One approach is to make an index card for each folder, then sort the cards — every additional folder means another card. The other approach is to stick a label directly on each folder — the label is part of the folder, no extra manufacturing needed. Sysoev's design is the latter. Adding a timer requires no extra memory allocation; removing one requires no deallocation. **Zero dynamic memory allocation.**

If this pattern looks familiar, it should — the previous post embedded the event object inside the connection object in exactly the same way. Intrusive nodes are Sysoev's signature style.

A personal note about this tree. Before joining the nginx team, I submitted a small improvement to Igor's red-black tree. Igor personally reviewed it and merged it. I'd been reading Igor's code for years. Contributing even a tiny piece to his codebase felt more meaningful than writing a thousand lines of my own. After joining the team, I confirmed what I'd already suspected: Igor's standards for code quality were extremely high. If he was willing to merge it, the change was right.

So bringing this tree into jsbench isn't just "picking a good implementation" — I'm intimately familiar with every line of it.

**Good selection isn't about choosing the newest or most popular option — it's about choosing what you understand and trust most deeply.** Trusting an implementation means you understand its design decisions, its edge cases, the conditions under which it might fail. That trust doesn't come from reading documentation. It comes from years of use and reading.

## Time Utility Functions

With the data structure in place, we still need time measurement.

Previously, jsbench used raw system calls to get the time everywhere — no unified time types, no wrapped clock functions. If the timer module were built on top of that, time-related details would leak into every line of timer code.

So we introduced a set of time types — seconds, milliseconds, and nanoseconds each with their own type names — plus three clock functions: wall clock, monotonic clock (for measuring intervals), and local time.

These are small — type definitions under 10 lines, function implementations under 30 lines. But they establish a boundary: **the timer module only deals with the "millisecond" abstraction, not how milliseconds are obtained from the operating system.** If the clock source, precision, or caching needs to change later, the time layer changes. The timer layer doesn't.

That's the point of layering — not for aesthetics, but so that changes in one layer don't ripple into others.

## The Timer Module

The red-black tree provides ordering. Time types provide measurement. The timer module assembles them into a complete interface:

- **Add** — register a timer to fire in x milliseconds
- **Delete** — cancel a timer
- **Find** — locate the nearest timer and return how many milliseconds until it fires
- **Expire** — check all expired timers and invoke their callbacks

These four operations are the timer's entire API. **Only these four actions are exposed; the red-black tree operations, time comparisons, and overflow handling are all hidden inside.** Users don't need to know whether timers are backed by a red-black tree or a min-heap — that's how infrastructure should be.

The module is self-contained — it knows nothing about connections, epoll, or jsbench. It only knows "there are some timers, sorted by time, call their callbacks when they expire." **Generic things should stay generic.**

## Five Layers: One-Way Dependencies

These infrastructure pieces form five independent layers:

```
js_unix.h     ← OS interfaces
js_clang.h    ← C language utilities
js_time.h     ← Time types and clock wrappers
js_rbtree.h   ← Red-black tree
js_timer.h    ← Timer module
```

Each layer depends only on layers below it and has no knowledge of layers above. The red-black tree doesn't know what a timer is. Time types don't know what a red-black tree is. OS interfaces don't know what jsbench is.

**Dependencies flow one way: top to bottom.**

This is the most important property of infrastructure. If the red-black tree depended on timer types, it would no longer be a generic red-black tree — it would be a "timer-specific red-black tree." If time types referenced the connection struct, they'd no longer be a generic time abstraction. One-way dependencies guarantee each layer's independence — it can be used by anyone, not tied to any specific scenario.

This principle isn't limited to C. In any language, any project, infrastructure dependencies should flow in one direction. Database utilities shouldn't depend on business logic. Logging modules shouldn't depend on the HTTP framework. **If your infrastructure "knows" about the layer above it, it's no longer infrastructure — it's part of that layer.**

## Splitting Headers: Not Early, Just on Time

While introducing this infrastructure, we did something we'd put off until now: split the header files.

Previously, all declarations lived in a single 350-line header file. I never split it — there was no need. Everything in it was jsbench-specific, and splitting would have meant dealing with include ordering. One file handling everything — that was the "just enough" from the fourth post.

But a red-black tree is different. It's a generic data structure that has nothing to do with business logic. Time types are the same. Stuffing generic things into a project-specific header is like filing a math textbook in a novel's table of contents — it doesn't belong there.

There was no reason to split before — "might need it later" is the classic starting point for over-engineering. Now there was a concrete need, and the direction was clear. **The best time to refactor isn't "when you have time" — it's when you have a concrete need.** The need tells you what to change and how far to go. Refactoring without a need tends to overshoot.

This is also a judgment AI wouldn't make on its own. AI would suggest "split headers by responsibility" on day one. But splitting when you have one header and no generic components is over-engineering. Splitting after introducing generic components is just right. **Timing matters as much as content.**

## What AI Did

AI did a large volume of execution work: adapting Sysoev's red-black tree implementation, writing time types and clock functions, implementing the timer's four interfaces, and splitting header files. Nearly 700 lines of new code across 10 files. This kind of work — mechanical, error-prone — is exactly where AI excels.

But the key judgments — red-black tree over min-heap, Sysoev's version over writing our own, splitting headers now instead of earlier — those were human decisions.

AI can quickly write a correct implementation after you say "use a red-black tree." But it won't proactively say "use Sysoev's version, because its intrusive design is a perfect match for jsbench's event model." It doesn't know how well you know this tree, how much you trust Sysoev's code, or that the choice is backed by a decade of reading and using it.

**Selection is a human judgment. The quality of selection depends on your experience, your taste, and how deeply you understand the problem domain.** These aren't things a prompt can teach AI.

Code changes: [ca705bf](https://github.com/hongzhidao/jsbench/commit/ca705bf)

At this point, the foundation was ready. The red-black tree provides ordering, time types provide measurement, the timer module provides the interface, and five header files keep each layer clean and independent. From a functionality standpoint, jsbench runs exactly as before — no timers unified, no change to `epoll_wait(100)`. But all the infrastructure is in place.

Next: **connect the new infrastructure to the existing system.**

## The Work Engine: epoll + Timers = One Complete Engine

The foundation was laid, but it was still disconnected — the red-black tree on one side, epoll on the other, no relationship between them. The timer module didn't know epoll existed, and epoll didn't know the timer module existed.

Back to the `epoll_wait(100)` problem: to replace that 100 with "how long until the nearest timer fires," epoll needs to query the timer module. That means they must be linked somewhere.

Where? The answer: **introduce a new struct that combines epoll and timers into a single unit.**

This is `js_engine_t` — the work engine.

If you're not familiar with programming, think of it this way. A car has an engine and a dashboard. The engine drives the vehicle; the dashboard shows time and speed. They can each exist independently, but only when installed in the same car can they work together — the dashboard tells you "5 kilometers to destination," and the engine knows whether to speed up or slow down. The work engine is that car: epoll is the engine (driving events), timers are the dashboard (managing time). Combined, they form a complete work engine.

Each worker thread has one engine. When the engine is created, it initializes both epoll and timers. When destroyed, it cleans up both. **All infrastructure for a thread's event-driven work lives in its engine.**

Alongside introducing the engine, we split the epoll interface into its own `js_epoll.h` header. At this point, the generic parts of `js_main.h` have gradually been extracted — OS interfaces, language utilities, time types, red-black tree, timers, epoll, work engine — each returning to its own module. `js_main.h` is slimming down, going from a catch-all dumping ground to containing only jsbench project-specific types and interfaces. **When modules multiply, letting each module's declarations live in their own place is the natural thing to do.**

## Reversing a Decision from the Previous Post

Here's the interesting part: introducing `js_engine_t` means reversing a decision from the previous post.

The previous post made the epoll file descriptor (epfd) a thread-local variable — each thread has its own copy, callers don't need to pass it. The reasoning was: epfd is thread-level infrastructure, not a parameter to be passed around.

Now epfd is back to being something that gets passed — it's inside the engine, and epoll operations require passing the engine in.

Is this contradictory?

No. What the previous post eliminated was **passing a raw epfd** — the caller had to remember an integer, and passing the wrong one wouldn't trigger a compile error. Now what's being passed is an **engine** — an object with clear semantics, representing "this thread's work engine." From passing a bare integer to passing an object: the level of abstraction went up.

More critically, the engine doesn't just hold epfd — it also holds timers. If we kept thread-local variables, timers would need to be thread-local too, and the association between epoll and timers would scatter across the codebase again. The engine gathers them together. **Keeping related things together is the most fundamental organizing principle.**

This also illustrates something: **a previous decision isn't necessarily wrong, but as new requirements emerge, better designs surface.** Introducing thread-local variables in the previous post was right — in that context, it solved the problem of epfd being passed everywhere. Introducing the engine now is also right — in this new context, epoll and timers need to be combined into a single unit. Architecture isn't built in one shot. It makes the most reasonable choice at each step, adjusting as the system evolves.

## Aggregation: A Practical Refactoring Technique

`js_engine_t` employs a highly practical refactoring pattern: **aggregation** — grouping related things into a single object.

This pattern sounds too simple to be worth mentioning, but it solves one of the most common forms of code messiness: **related things scattered across different places.**

Before the engine, epoll and timers were each on their own. The epoll file descriptor was hidden in a thread-local variable. The timer data structure had no home. The relationship between them was implicit — "epoll's timeout should be determined by timers" existed only in the programmer's head, not in the code.

After the engine, that relationship became explicit. Epoll and timers live in the same struct. Their association is expressed by the code itself — not by comments, not by documentation, not by verbal agreements. **When relatedness is expressed in code rather than remembered by humans, the system is less prone to errors.**

This pattern applies far beyond C. In any project, when you find several variables that always appear together — created together, passed together, destroyed together — they probably should be aggregated into one object. Database connection and pool config always used together? Aggregate into a `DatabaseClient`. User ID and permission list always passed together? Aggregate into a `UserContext`. HTTP request and its timeout config always paired? Aggregate into a `RequestOptions`.

Aggregation isn't just "grouping for convenience." It has two deeper benefits.

**First, encapsulation.** Scattered things have no boundary — anyone can directly manipulate them. Once aggregated, access can be controlled through interfaces. The engine only exposes create and destroy. How it initializes epoll, how it initializes timers — the outside doesn't need to know.

**Second, unified lifecycle.** Scattered things easily lead to "created A but forgot to create B," "destroyed A but forgot to destroy B." Once aggregated, one create readies everything, one destroy cleans up everything. **When paired operations live together, nothing gets forgotten.**

There's a simple signal for when to aggregate: **if you find yourself repeatedly passing the same group of parameters in multiple places, or repeating the same initialization/cleanup steps in multiple places, that's the time.** Just like this case — both worker and loop had to create epoll, init timers, and close epoll. When that pattern repeats, it's time to pack them into one object.

## AI and Refactoring

Introducing `js_engine_t` — the architectural judgment took only minutes: epoll and timers should be combined into one object, each thread holds one. But applying that judgment to code touched 6 files: removing the thread-local variable, creating engine create/destroy functions, adding the engine parameter to every epoll call, and updating both worker and loop creation and cleanup flows.

Hand this kind of work to AI, and it does exceptionally well — **ridiculously well**, to be frank.

AI has one advantage in refactoring that humans can barely match: **it doesn't miss anything.** Manually changing 20 function call sites — you'll probably miss one, catching it at compile time if you're lucky, or worse, at runtime. AI scans the code and changes every call site in one pass — signatures aligned, parameters consistent, nothing missed. This isn't "faster than a human" — it's "more reliable than a human."

The previous post made a similar point — zero mistakes on mechanical changes, cross-file consistency. But this time the feeling was stronger. Because this change **reversed a previous design**: turning thread-local variables back into explicit parameters. That means the code AI changed last time is now being changed by AI again — not reverted exactly, but evolved to a new abstraction level. AI shows no reluctance. It doesn't hesitate or cut corners because "we just changed this." It treats every change as a fresh task and executes it mechanically and completely.

**Refactoring is one of the most valuable use cases for AI programming today.** The reason is simple: refactoring is characterized by clear direction, broad scope, and highly mechanical work — exactly what AI excels at. Humans provide the direction ("introduce engine, combine epoll and timer"), and AI applies that direction to every line of code in every file. If the direction is wrong, the refactoring is wasted. If the direction is right, AI's execution means you spend zero time on mechanical labor.

**One architectural judgment from you, and AI applies it to every line across dozens of files. The more accurate the judgment, the bigger the leverage.**

Code changes: [8f25458](https://github.com/hongzhidao/jsbench/commit/8f25458)

## Takeaways

This post did two things.

First: lay the foundation. Introduce the red-black tree, time utility functions, and a standalone timer module, forming five layers of infrastructure with one-way dependencies. Functionality unchanged, but all the groundwork for what comes next is in place.

Second: introduce `js_engine_t`, combining epoll and timers into a complete work engine. One engine per thread. The engine owns all of that thread's infrastructure.

Both follow the same theme: **build the structure first, then the features.**

The 100 in `epoll_wait(100)` still hasn't changed. The three timer mechanisms still aren't unified. But now the engine holds both epoll and timers — making `epoll_wait` dynamically compute its timeout from the nearest timer is just one step away. **When the structure is right, features fall into place naturally.**

**The foundation isn't the goal, but without it, the goal can't be reached.** Many people think infrastructure is a waste of time — "spent all this effort and functionality didn't change at all." But good infrastructure makes every subsequent step simpler and more solid. Conversely, skipping the foundation and jumping straight to features produces something fragile, duplicated, or impossible to change.

That red-black tree isn't ours. Igor Sysoev wrote it over 20 years ago. **Good architecture isn't just about design — it's about selection.** Knowing when to build your own and when to use what someone else already built. If a problem has already been solved by the best engineer in the best possible way, your job isn't to surpass them. It's to understand their design and put it in the right place.

---

GitHub: https://github.com/hongzhidao/jsbench
