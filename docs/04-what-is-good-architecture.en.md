# What Is Good Architecture

The previous posts were about fixing problems and changing code. This one steps back to think: what does good architecture actually look like.

If I had to answer "what is architecture" in one sentence, my understanding is: **architecture is how you manage complexity.**

## Managing Complexity

A system is as complex as what it needs to do — that's objective. Architecture doesn't eliminate complexity; it keeps complexity under control — so it doesn't spiral out of hand as features grow, and a single change doesn't break everything.

## Putting Complexity in Boxes

Good architecture puts complexity into boxes. Each box exposes a simple interface; the internal complexity is hidden. When using a module, you don't need to know how it's implemented — just its interface.

But hiding is not transferring.

**Transferring** moves complexity from one place to another without truly solving it. The most common example is unreasonable dependencies — A depends on B, B depends on A, forming a cycle. Someone adds a module C as a middle layer. The cycle looks gone, but C is just a dumping ground — all the things that shouldn't be coupled get shoved into it. Complexity hasn't disappeared; it just found a new hiding spot.

Real hiding means the inside of the box is also clean.

This matters — because modules aren't sealed forever. You'll open them someday: when debugging, optimizing, or changing internal logic as requirements evolve. If the inside is clean, you open it, make the change, close it back up. If the inside is a mess, opening it is like opening Pandora's box — complexity floods out, and you're afraid to touch anything because you don't know what might break.

So good hiding works on two levels: **simple on the outside, clean on the inside.**

## Solving a Class of Problems

Good architecture provides a framework, not a point solution for a specific requirement.

nginx's event loop wasn't designed to "handle HTTP requests" — it solves the class of problems around "efficiently managing large numbers of concurrent connections." That's why the same architecture handles HTTP, mail, and stream. jsbench's js_loop.c is similar — it wasn't designed to "make fetch concurrent" — it solves the class of problems around "asynchronously driving multiple I/O operations in QuickJS."

## What Good Architecture Achieves

Good architecture ultimately achieves three things:

**Stability.** Adding a feature, complexity grows only locally — it doesn't spread across the entire system. nginx can have hundreds of modules while overall complexity stays manageable, because the module system confines each feature's complexity to its own scope.

**Simplicity for users.** Writing an nginx module, you don't need to know how the event loop works. Calling fetch() in jsbench, you don't need to know about epoll and connection state machines. Good architecture makes every layer's users feel "this is simple."

**Locality.** Understanding one module doesn't require understanding the entire system. Modifying one module doesn't ripple into others. Developers' attention is finite — good architecture directs that finite attention where it matters most.

But achieving these effects requires getting the design just right. Both under-design and over-design hurt complexity.

**Under-design** lets complexity leak everywhere. No module boundaries, no interface abstractions, all code tangled together. Adding a feature requires understanding half the system; changing one spot might break who-knows-what. The first version of jsbench's fetch() was an example — network I/O, DNS resolution, event loop, Promise wrapping all crammed into one 400-line function with no layering.

**Over-design** adds complexity out of thin air. The problem is simple, but for "extensibility" you add three abstraction layers, two interfaces, and a factory pattern. Code volume triples, but it still solves the same problem. Worse, these abstractions often don't help when requirements actually change — because you guessed the wrong direction.

Good architecture sits between the two: **just enough, no more, no less.**

> "Make it work, make it right, make it fast." — Kent Beck

I quoted this in the second post, but only expanded on "make it work" and "make it right." Let me complete the picture with "make it fast." Make it fast isn't just about performance optimization — more precisely, it's about solving performance problems after the first two steps are done: algorithm improvements, data structure tuning, reducing memory allocations. The key is the **order**: get it right first, then make it fast. Optimizing a system with clean architecture is far easier than optimizing a mess.

The core of these three steps is really "make it right." Work is the starting point, fast is the icing on the cake, but right is what determines how far a piece of software can go.

In my experience, the engineers in the nginx community are all very good at make it right and make it fast. Igor Sysoev is especially skilled at simplifying complex problems through design — the architecture of nginx and unit are the best examples. Next, I plan to bring nginx unit's event engine into jsbench.

Of course, knowing is the easy part — good architecture is ultimately forged through practice.

---

GitHub: https://github.com/hongzhidao/jsbench
