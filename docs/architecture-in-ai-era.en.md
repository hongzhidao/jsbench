# What Architecture Is, and What It Means in the AI Era

> After ten refactorings, I have a clearer understanding of architecture than ever before.

## An Experiment

A month ago I ran an experiment: let AI write a C project from scratch — a programmable HTTP benchmarking tool built with C + QuickJS. About two thousand lines, working in a day.

Then I spent weeks refactoring its architecture. Ten times. Each refactoring became an article.

Throughout the process, I kept returning to the same question: **what exactly is architecture?** Not the textbook definition — but when you're reviewing real code and making real decisions, what does the word actually mean?

After ten refactorings, I feel I understand the word better than before. Not because I read more theory, but because AI-generated code gave me the perfect counter-example — it demonstrated "the absence of architecture" more vividly than any textbook could.

## What AI's Code Taught Me About Architecture

AI-written code has a distinctive trait: **every piece looks fine in isolation, but the whole thing resists change.**

Accurate naming, consistent prefixes, passing tests, working features. You can't point to a single function that's "bad." But when you try to add a feature or change a behavior, everything is tangled — changing one place might break another, and adding anything requires understanding half the system.

Why? Because **complexity is uncontrolled.**

Article four in the series discussed how architecture is fundamentally about managing complexity. That article was more theoretical. Now I want to use AI's actual code to make the lesson concrete.

### What "Put It in a Box" Really Means

Article four said good architecture puts complexity "into boxes" — simple on the outside, clear on the inside.

That sounds like a correct platitude. But when I saw AI's `js_fetch.c`, I suddenly understood what "not in a box" feels like in practice.

AI's fetch implementation was a 400-plus-line function: network I/O, DNS resolution, creating an independent epoll instance, synchronously waiting for the response, wrapping a Promise — all crammed together. No layering, no interfaces. **Five different kinds of complexity churned in a single function.**

The result: wanting to add concurrency to fetch was impossible. Because fetch managed its own independent epoll, disconnected from the global event loop. To add concurrency, you'd have to simultaneously change the I/O model, the Promise mechanism, and the event loop — because they were never separated, they could only be changed together.

After refactoring, fetch only creates a connection, registers it with the global event loop, and returns a pending Promise. The event loop's complexity lives in `js_epoll.c`, the Promise mechanism in `js_loop.c`, connection management in `js_conn.c`. Each "box" exposes only a simple interface. Want to change concurrency? Just modify the event loop box — the other boxes don't need to be touched.

**"Put it in a box" isn't a metaphor. It's a very concrete engineering action: give each kind of complexity its own boundary, so it doesn't mix with other kinds of complexity.** When you achieve this, changing one place doesn't threaten another. When you don't, you're afraid to touch anything.

### What "Hiding Isn't Transferring" Really Means

Article four also drew a distinction: hiding complexity and transferring complexity are not the same thing. It used a "dumpster" analogy. In AI's code, I saw a more vivid real-world example.

`js_conn_t` — the connection struct — had a `js_http_response_t` embedded inside it. The connection layer's read function directly called the HTTP parser. `js_conn_keepalive()` read HTTP headers to decide on connection reuse.

On the surface, HTTP logic appeared "hidden inside" the connection module. But this wasn't hiding — it was transferring. HTTP's complexity was stuffed into a place that shouldn't bear it. The result: the connection layer couldn't exist independently of HTTP, changing HTTP parsing meant modifying connection code, and changing connection strategy might break HTTP logic. **Two layers' complexity wasn't additive — it was multiplicative.**

After refactoring, `js_conn_t` contains zero HTTP fields. The connection layer only handles transport: establishing connections, reading bytes, writing bytes. HTTP parsing, keep-alive decisions, response handling — all live in the caller. The connection doesn't know HTTP exists, just as the event engine doesn't know connections exist.

**Real hiding means each module bears only the complexity it should bear. If a module carries something that doesn't belong to it, that's not hiding — it's a ticking time bomb.**

### What "Just Enough" Really Means

Article four said good architecture sits between under-design and over-design: just enough, no more, no less.

In AI's code, under-design was everywhere: timers had three completely different implementations (C-path used timerfd, JS-path manually compared timestamps, request timeouts iterated through all pending requests), the same job done three times; the request concept was split into two types (`js_request_desc_t` and `js_raw_request_t`), with duplicated data and unclear ownership.

But during refactoring, the temptation to over-design was equally present. AI would suggest "split header files by responsibility" on day one — but with only one header file and no reusable components, splitting would have been over-design. Later, when we introduced a red-black tree and timer module that didn't belong to any business logic — that's when splitting was exactly right.

**"Just enough" isn't a design skill — it's a timing judgment.** The same action, done too early, is over-design; done too late, is under-design. When to act depends on your current understanding of the system — and understanding deepens through the process of solving problems.

This is also why good architecture isn't designed upfront — it's grown step by step. None of jsbench's ten refactorings were planned in advance. Each problem solved revealed the next problem. Trying to encapsulate `epoll_wait`, we found `epfd` was a dependency. Eliminating `epfd`, we found conn was coupled into the event loop. Decoupling conn, `js_epoll_poll()` finally fell into place naturally.

## In the AI Era, Architecture's Value Is Amplified

Everything above describes the essence of architecture — true with or without AI. But AI causes a qualitative shift in architecture's value.

Why? Because AI changes a critical economic equation: **the cost of writing code approaches zero, but the cost of managing complexity hasn't changed.**

Before AI, writing code was the bottleneck. Ideas were plentiful, but turning them into code took time. So "being able to turn ideas into code" was itself a scarce skill.

Now AI can write code. Two thousand lines of C in a day — working, tested, reasonably named. Writing code is no longer the bottleneck.

So what became the bottleneck?

**Complexity.**

AI writes code far faster than humans. But more code means more complexity. Without architecture to manage that complexity, the faster AI writes, the faster the system becomes unmaintainable spaghetti.

This is exactly what I saw with jsbench. AI wrote two thousand lines of working code in a day. But complexity was uncontrolled: the connection layer embedded an HTTP parser, fetch was pseudo-async, timers had three implementations, the request type was split in two. Everything worked, but the code couldn't evolve.

**AI dramatically reduces the cost of producing code, but simultaneously accelerates the production of complexity.** Without architecture to contain it, AI is an efficient technical debt generator.

Conversely: **when the architecture is right, AI becomes your entire team.**

I told AI "fetch should return a pending Promise, with I/O driven by the global event loop" — it got it right in one pass, across 9 files. I said "each thread has exactly one epoll" — it immediately rewrote all interfaces using thread-local storage. I said "this field doesn't belong in conn, it belongs to a new concept called http_peer" — it created the new struct, moved fields, and updated every reference.

Every time, AI's execution was impeccable. It never missed a change, never got a signature wrong, never forgot to update a caller. Six files, twenty-plus modifications, done in one pass.

But every time, the prerequisite was: **I gave it the correct direction.**

Where does direction come from? From understanding architecture. Knowing what proper layering looks like, knowing which way dependencies should flow, knowing where complexity should be hidden.

**Architecture is the prerequisite for AI to work effectively.** Without architectural direction, AI can only spin in place — it'll list options but won't commit to a direction. With architectural direction, AI becomes a precision execution machine.

So in the AI era, architecture isn't "nice to have." It's **essential.** Code can be written by AI, but architecture can't — because architecture determines whether AI-written code is an asset or a liability.

## Four Fundamental Moves

Across ten refactorings, four moves recurred repeatedly. They aren't specific to systems programming — they apply wherever complexity needs to be managed.

**Define boundaries.** Draw a line with abstraction — everything above the line is treated uniformly, everything below is different. jsbench's event engine works this way: connections are events, timers are events, dispatch logic doesn't care about the difference — it just calls handlers. Adding new event types requires zero changes to dispatch code. This is the fundamental reason systems can scale.

**Eliminate dependencies.** If something is infrastructure, don't make everyone carry it around. The epoll file descriptor is a thread attribute, not a parameter to pass. One fewer parameter isn't just less typing — it eliminates the possibility of passing the wrong one at the source.

**Decouple.** Don't work around the problem — make each module's logic return to where it belongs. The connection layer shouldn't know about HTTP. The event engine shouldn't know about connections. When you want to encapsulate a function but can't, it's usually because coupling is in the way — decouple first, then encapsulate.

**Hide complexity.** Put implementation details inside a box. Expose only a simple interface. `epoll_wait()` went from scattered across two files to existing solely inside `js_epoll_poll()`. Complexity doesn't disappear, but it can be isolated so it doesn't spread throughout the system.

The common essence of these four moves: **managing complexity.** Defining boundaries scopes complexity. Eliminating dependencies reduces its transmission paths. Decoupling prevents it from compounding. Hiding it prevents it from spreading.

When code feels impossible to change — whether it's your own or AI-generated — one of these four directions usually hasn't been addressed.

## The Economics of Iteration in the AI Era

One more thing worth discussing: AI changed the economics of iteration.

Before, a single refactoring might take a senior engineer a full day. Ten refactorings, two weeks. Many times you know the code isn't good enough, but the cost of fixing it is too high, so you tolerate it. That's how technical debt is born.

Now? Explain the direction to AI, one refactoring in minutes. It won't hesitate because "we just changed this." It won't modify an interface and forget a call site. It won't miss one out of twenty changes.

**Architectural improvements you couldn't afford before, you can afford now.**

What does this mean? It means **the bar for "just enough" can be higher.** The "good enough" you tolerated before because the cost of change was too high — now you can achieve genuinely "just right." Those TODO items you shelved — unnecessary dependencies, blurry boundaries, fields that don't belong — can be fixed continuously.

**AI turns architecture evolution from "we'll get to it someday" into something you can do continuously.**

This is AI's most profound impact on programming. Not "AI can write code" — that's the surface. The real change: **the barrier to good architecture has dropped.** Previously, only large companies' core projects could afford continuous architectural refinement. Now one person plus AI can do it.

But iteration costs went down. Iteration direction is still on you.

## Which Skills Are Appreciating

This brings us to the question many are asking: **what are the core skills for programmers in the AI era?**

**Architectural judgment.** AI can implement any direction, but won't choose the direction for you. How code is organized, how modules are split, where abstractions live, where complexity should be hidden — these decisions determine whether AI-written code is an asset or liability. One architectural judgment, applied by AI across dozens of files. If the judgment is right, that's leverage at scale. If wrong, also leverage at scale.

**Code review.** AI writes code faster than humans, and produces bugs faster too. The ability to see what's wrong — not syntax errors, but logic and architecture-level problems — isn't a nice-to-have in the AI era. It's the **last line of defense.** AI-written code and AI-written tests share blind spots. I've experienced this firsthand.

**Domain depth.** I could judge that fetch should use an event loop instead of synchronous blocking not because my prompts were well-crafted, but because I've been writing event-driven systems for over a decade. AI amplifies capabilities you already have. It doesn't create them from nothing. The deeper you are in a domain, the greater the leverage.

**Refactoring ability.** Martin Fowler's *Refactoring*, or Joshua Kerievsky's *Refactoring to Patterns*. Refactoring is the most valuable programming activity in the AI era — clear direction, broad scope, mechanical in nature — exactly what AI excels at. You identify problems and judge direction. AI applies changes to every line of code.

As for "writing code" itself? Its weight is declining. Not that it's unnecessary — it's just no longer the bottleneck. **The bottleneck shifted from "how to turn ideas into code" to "how to manage the complexity code produces."** And managing complexity is architecture.

## In the End

Back to the original definition: **architecture is how you manage complexity.**

Before AI, this meant: good architecture makes team collaboration more efficient and maintenance costs lower. Important, but not life-or-death.

After AI, the weight of this sentence changes: **architecture is the only barrier preventing AI's productivity from becoming destructive.**

AI can write all the code. But writing code isn't the goal — managing complexity is. The more code AI can write, the faster complexity accumulates, and the more valuable architecture becomes.

This is a counterintuitive conclusion: **the easier AI makes writing code, the more important architecture becomes.** Not less important — more.

The future of programmers isn't writing code. It's making judgments. Judging what the right direction is. Judging where the code is wrong. Judging where complexity should be hidden. Then letting AI apply those judgments to every line of code.

Judgment comes from architectural understanding, domain depth, and sustained practice. There are no shortcuts.

But the good news is: AI makes every ounce of your judgment more valuable than ever before.

---

*This article is based on the ten hands-on articles in the series "An nginx Engineer Took Over AI's Benchmark Tool." The complete series covers the full architectural refactoring process, from code organization to event engines, from connection layering to struct design. Each article includes links to the corresponding code changes.*

GitHub: https://github.com/hongzhidao/jsbench
