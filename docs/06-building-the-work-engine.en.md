# Nothing Changed, but the Engine Is Built

The last post encapsulated the event engine into a single line: `js_epoll_poll(100)`. This post introduces jsbench's most central structure: the work engine.

The end result: a struct called `js_engine_t` — one per thread, holding both event dispatch (epoll) and timer management. A thread's complete work engine. But this result didn't come in one step — we spent an entire commit laying the foundation, then discovered the foundation alone wasn't enough — the pieces were disconnected and needed to be assembled into a whole.

## Three Timers, One Job

Let's start with the problem.

jsbench has three completely different timer mechanisms. The C-path uses an OS-provided timer file descriptor — when time's up, epoll receives an event. The JS-path manually checks the current time at the start of each loop iteration, compares it against the deadline, and exits if it's past. Request timeouts live in the event loop — each iteration walks all pending requests and checks them one by one.

Three approaches doing the same thing: "wait a while, then do something." But their implementations are entirely different, and none knows the others exist. To unify them, we need a generic timer module. But a timer module needs data structures, time types — infrastructure. **You can't build a house on sand.**

## Choosing the Data Structure: Red-Black Tree, Not Min-Heap

A timer needs three operations: add ("wake me in x milliseconds"), delete ("never mind, the request already completed"), and find the nearest ("when does the next alarm go off?").

Imagine you're managing a pile of to-do items, each with a deadline. You need to know which one is most urgent at any time, add new ones quickly, and cross off completed ones quickly.

**Simplest approach: a random pile of papers.** Find the most urgent one? Flip through the entire pile. That's what the current request timeout does.

**Red-black tree: a self-sorting filing cabinet.** Items go in sorted automatically. The most urgent is always at the front. Adding, removing, checking the front — every operation is fast. 100 items: at most 7 comparisons. 1,000: at most 10. 65,536: at most 16.

There's another common choice called a min-heap — checking the front takes just 1 step, and adding is fast too. But there's a catch: if you need to cross off something that's not at the front (say a request completed early and you want to cancel its timeout), a min-heap has to search through the pile to find it. Red-black trees don't have this problem — you're holding a reference to that item, just remove it directly. **The red-black tree has no weak spot across all three operations.**

AI would suggest a min-heap — that's what the textbooks teach. But textbook scenarios typically don't require frequent deletion of arbitrary nodes. jsbench does. **Selection isn't about choosing the most common option — it's about choosing what fits your scenario best.**

Data structure decided. Next question: whose implementation? Red-black tree implementations involve rotations and recoloring, with error-prone details — not something you need to write yourself. Igor Sysoev implemented one in 2002 when writing nginx. Over the past 20-plus years, that tree has been battle-tested on hundreds of millions of connections worldwide. He later wrote an improved version for njs (nginx's JavaScript engine) — more flexible, more generic. In my view, it's the best red-black tree implementation there is.

The tree's most important design feature is **intrusive nodes** — the tree node isn't a separately allocated object; it's embedded directly into the user's struct. If you're not familiar with programming, think of it this way: to sort folders by date, one approach makes an index card per folder, the other sticks a label directly on the folder. Sysoev's design is the latter. Adding a timer requires no extra memory allocation; removing one requires no deallocation. **Zero dynamic memory allocation.** The previous post embedded the event object inside the connection object in exactly the same way.

A personal note about this tree. Before joining the nginx team, I submitted a small improvement to Igor's red-black tree. Igor personally reviewed it and merged it. I'd been reading Igor's code for years. Contributing even a tiny piece felt more meaningful than writing a thousand lines of my own. So bringing this tree into jsbench isn't just "picking a good implementation" — I'm intimately familiar with every line of it.

**Good selection isn't about choosing the newest or most popular — it's about choosing what you understand and trust most deeply.** That trust doesn't come from reading documentation. It comes from years of use and reading.

## Building It Up Layer by Layer

With the red-black tree in place, we still needed time measurement. Previously, jsbench used raw system calls everywhere to get the time — no unified time types. If the timer module were built on that, time-related details would leak into every line of code. So we introduced a set of time types and three clock functions. Small — types under 10 lines, functions under 30 — but they established a boundary: **the timer module only deals with the "millisecond" abstraction, not how milliseconds are obtained from the OS.**

Then the timer module itself. The red-black tree provides ordering, time types provide measurement. The timer module assembles them into four operations: add, delete, find the nearest, expire all due timers. Only these four actions are exposed; the red-black tree operations and overflow handling are all hidden inside. The module is self-contained — it knows nothing about connections, epoll, or jsbench. **Generic things should stay generic.**

While introducing this infrastructure, we split the header files. Previously, all declarations lived in a single 350-line header. I'd never split it — there was no need. But a red-black tree is a generic data structure with no business logic; time types are the same. Stuffing generic things into a project-specific header is like filing a math textbook in a novel's table of contents — it doesn't belong there. **The best time to refactor isn't "when you have time" — it's when you have a concrete need.** AI would suggest "split headers by responsibility" on day one — splitting with no generic components is over-engineering; splitting after introducing generic components is just right. **Timing matters as much as content.**

The result: five independent layers.

```
js_unix.h     ← OS interfaces
js_clang.h    ← C language utilities
js_time.h     ← Time types and clock wrappers
js_rbtree.h   ← Red-black tree
js_timer.h    ← Timer module
```

Each layer depends only on layers below it and has no knowledge of layers above. **Dependencies flow one way: top to bottom.** This is the most important property of infrastructure. In any language, any project, infrastructure dependencies should flow in one direction. Database utilities shouldn't depend on business logic. Logging modules shouldn't depend on the HTTP framework. **If your infrastructure "knows" about the layer above it, it's no longer infrastructure — it's part of that layer.**

Code changes: [ca705bf](https://github.com/hongzhidao/jsbench/commit/ca705bf)

## Foundation Laid, but the Pieces Are Disconnected

At this point: nearly 700 lines of new code, 10 files. Run jsbench — exactly the same as before. No timers unified. The 100 in `epoll_wait(100)` is still there.

The foundation was indeed ready. But the red-black tree was on one side, epoll on the other, no relationship between them. The timer module didn't know epoll existed, and epoll didn't know the timer module existed.

Back to the 100 problem: to replace it with "how long until the nearest timer fires," epoll must be able to query the timer module. **They must be linked somewhere.**

## The Work Engine

Where to link them? The answer: **introduce a new struct that combines epoll and timers into a single unit.**

This is `js_engine_t` — the work engine.

If you're not familiar with programming, think of it this way. A car has an engine and a dashboard. The engine drives the vehicle; the dashboard shows time and speed. They can each exist independently, but only when installed in the same car can they work together — the dashboard tells you "5 kilometers to destination," and the engine knows whether to speed up or slow down. The work engine is that car: epoll is the engine (driving events), timers are the dashboard (managing time). Combined, they form a complete work engine.

Each worker thread has one engine. When the engine is created, it initializes both epoll and timers. When destroyed, it cleans up both. **All infrastructure for a thread lives in its engine.**

Alongside introducing the engine, we split the epoll interface into its own `js_epoll.h` header. The generic parts of `js_main.h` have now been gradually extracted — OS interfaces, language utilities, time types, red-black tree, timers, epoll, work engine — each returning to its own module. **When modules multiply, letting each module's declarations live in their own place is the natural thing to do.**

Here's the interesting part: introducing the engine means reversing a decision from the previous post. The previous post made the epoll file descriptor a thread-local variable — each thread has its own copy, callers don't need to pass it. Now it's back to being something that gets passed — it's inside the engine, and epoll operations require passing the engine in.

Is this contradictory? No. What the previous post eliminated was **passing a raw integer** — pass the wrong one and the compiler won't complain. Now what's being passed is an **engine** — an object with clear semantics, representing "this thread's work engine." The level of abstraction went up. More critically, the engine doesn't just hold epoll — it also holds timers. If we kept thread-local variables, timers would need to be thread-local too, and the association between epoll and timers would scatter across the codebase again. The engine gathers them together. **A previous decision isn't necessarily wrong, but as new requirements emerge, better designs surface.** Architecture isn't built in one shot. It makes the most reasonable choice at each step, adjusting as the system evolves.

## Aggregation: A Practical Refactoring Technique

The pattern behind the engine is worth calling out: **aggregation** — grouping related things into a single object.

This sounds too simple to be worth mentioning, but it solves one of the most common forms of code messiness: **related things scattered across different places.** Before the engine, the relationship between epoll and timers was implicit — "epoll's timeout should be determined by timers" existed only in the programmer's head, not in the code. After the engine, that relationship is explicit — they live in the same struct, and their association is expressed by the code itself. **When relatedness is expressed in code rather than remembered by humans, the system is less prone to errors.**

This pattern applies far beyond C. In any project, when several variables always appear together — created together, passed together, destroyed together — they probably should be aggregated into one object. Database connection and pool config always used together? Aggregate into a `DatabaseClient`. User ID and permission list always passed together? Aggregate into a `UserContext`.

Aggregation isn't just "grouping for convenience." **Encapsulation** — scattered things have no boundary; once aggregated, access can be controlled through interfaces. **Unified lifecycle** — one create readies everything, one destroy cleans up everything. No more "created A but forgot to create B."

There's a simple signal for when to aggregate: **if you find yourself repeatedly passing the same group of parameters in multiple places, or repeating the same initialization/cleanup steps in multiple places, that's the time.** Just like this case — both worker and loop had to create epoll, init timers, and close epoll. When that pattern repeats, it's time to pack them into one object.

Code changes: [8f25458](https://github.com/hongzhidao/jsbench/commit/8f25458)

## AI's Role

Across both changes, AI's role was always the same: humans make judgments, AI executes.

The first change — laying the foundation — the key judgment was selection. Red-black tree over min-heap. Sysoev's version over writing our own. Splitting headers now instead of earlier. AI can quickly write a correct implementation after you say "use a red-black tree," but it won't proactively say "use Sysoev's version, because its intrusive design is a perfect match for jsbench's event model." It doesn't know how well you know this tree, or that the choice is backed by a decade of reading. **The quality of selection depends on your experience, your taste, and how deeply you understand the problem domain.** These aren't things a prompt can teach AI.

The second change — building the engine — the key judgment was architecture: epoll and timers should be combined into one object, each thread holds one. But applying that judgment to code touched 6 files. Hand this kind of work to AI, and it does **ridiculously well**.

AI has one advantage in refactoring that humans can barely match: **it doesn't miss anything.** Manually changing 20 function call sites — you'll probably miss one. AI scans the code and changes every call site in one pass — signatures aligned, parameters consistent, nothing missed. This isn't "faster than a human" — it's "more reliable than a human."

The feeling was especially strong this time — because this change **reversed a previous design**: turning thread-local variables back into explicit parameters. The code AI changed last time now gets changed by AI again — evolved to a new abstraction level. AI shows no reluctance, doesn't hesitate because "we just changed this." It treats every change as a fresh task and executes it mechanically and completely.

**Refactoring is one of the most valuable use cases for AI programming today.** Clear direction, broad scope, highly mechanical — exactly what AI excels at. Humans provide the direction, AI applies it to every line of code. **One architectural judgment from you, and AI applies it to every line across dozens of files. The more accurate the judgment, the bigger the leverage.**

## Takeaways

Looking back at what this post did: introduced the red-black tree, time types, and timer module to lay a five-layer foundation; then introduced `js_engine_t` to assemble epoll and timers into a complete work engine. One engine per thread. The engine owns all of that thread's infrastructure.

The 100 in `epoll_wait(100)` still hasn't changed. The three timer mechanisms still aren't unified. But now the engine holds both epoll and timers — making `epoll_wait` dynamically compute its timeout from the nearest timer is just one step away. **When the structure is right, features fall into place naturally.**

**The foundation isn't the goal, but without it, the goal can't be reached.** Many people think infrastructure is a waste of time — "spent all this effort and functionality didn't change at all." But good infrastructure makes every subsequent step simpler and more solid. Conversely, skipping the foundation and jumping straight to features produces something fragile, duplicated, or impossible to change.

That red-black tree isn't ours. Igor Sysoev wrote it over 20 years ago. **Good architecture isn't just about design — it's about selection.** Knowing when to build your own and when to use what someone else already built. If a problem has already been solved by the best engineer in the best possible way, your job isn't to surpass them. It's to understand their design and put it in the right place.

---

GitHub: https://github.com/hongzhidao/jsbench
