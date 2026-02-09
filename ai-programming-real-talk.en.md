# AI Wrote My Project, I Refactored Its Architecture

I've been writing code for over a decade. I work on the nginx team — nginx, njs, nginx unit. Recently I ran an experiment: I had AI (Claude) write a programmable HTTP benchmarking tool from scratch. C + QuickJS, around 2,000 lines. It was functional in a day.

Then I spent weeks refactoring its architecture, one commit at a time.

What I learned doesn't quite match the prevailing narratives about AI programming — neither the "AI will replace us all" camp nor the "it's just hype" camp.

## The most dangerous AI bugs are invisible

During refactoring, I found something unsettling.

jsbench has a mode where users write JS scripts that call `fetch()` to generate HTTP traffic. AI wrote this feature. AI also wrote the tests. Test report: **16,576 requests, 0 errors. PASS.**

When I reviewed the code, I discovered: **every single fetch had failed.**

The worker thread had no event loop. `fetch()` couldn't actually send anything. But the code unconditionally counted every invocation as a success. 16,576 requests, zero successful, report says zero errors.

Here's what happened: **AI-written code and AI-written tests shared the same blind spot.** The implementation skipped error checking. The tests skipped error validation. They complemented each other perfectly, producing a flawless illusion that everything worked.

This is the most dangerous class of AI bugs: not crashes, not compilation errors, but **programs that run normally, pass all tests, and produce entirely wrong results.**

If you use AI to write code and also use AI to write the tests for that code, keep this in mind: they may be blind in exactly the same place.

## With the right direction, AI is your entire team

But there's another side to this story.

I found that `fetch()` didn't support concurrency. `Promise.all` with three requests that should complete in ~300ms was taking ~900ms. AI had implemented "fake async" — the function signature returned a Promise, but internally it was synchronous and blocking.

I knew the correct approach: `fetch()` should register the connection with a global event loop, return a pending Promise, and let the event loop drive all I/O. This isn't novel — it's the standard pattern in every high-performance async I/O framework.

I told AI four things: what the problem was, what the correct architecture looked like, what existing code it could build on, and what the constraints were.

It got it right in one shot. Nine files changed. 905ms → 302ms.

If I had just said "fetch has a bug," AI probably would have patched around the existing architecture. But because I could give it a clear architectural direction, it made the correct structural change in one pass.

**The ceiling of what AI can do is determined by the quality of direction you give it.**

## Judgment is the real multiplier

More refactoring made this pattern clearer.

At one point I decided to combine epoll and timers into a single "engine" object — one per thread, owning all the infrastructure for that thread. Simple idea. But implementing it touched 6 files and over 20 call sites. AI changed every one, without missing a single instance.

Conversely, if that judgment had been wrong — if combining them was the wrong call — AI would have applied the mistake just as thoroughly, across every file, with the same diligence.

**One architectural judgment, applied by AI across dozens of files. If the judgment is right, that's enormous leverage. If it's wrong, the leverage works against you just as hard.**

## What skills become more valuable

There's a lot of discussion about whether AI will replace programmers. Vibe coding, AI agents, generating apps from a single prompt — it sounds like there won't be much left for humans to do.

Based on hands-on experience, I think the question is framed wrong. **AI doesn't change whether programmers are useful. It changes which programmers are more useful.**

**Architectural judgment.** AI can implement any direction, but it won't choose the direction for you. How to organize code, where to draw module boundaries, what level of abstraction to use — these matter more in the AI era, not less. Get the direction right, and AI's execution amplifies your judgment. Get it wrong, and it amplifies your mistake.

**Code review.** AI writes code far faster than humans. It also produces bugs far faster than humans. The ability to spot what's wrong — not syntax errors, but logic and architecture-level problems — is now a defensive necessity, not a nice-to-have.

**Domain depth.** I could tell that `fetch()` needed an event loop instead of synchronous blocking not because I wrote a good prompt, but because I've spent over a decade writing event-driven systems. **AI amplifies the abilities you already have. It doesn't create them from nothing.** The deeper your expertise in a domain, the more leverage AI gives you. If you know a little about everything but nothing deeply, AI amplifies that shallowness.

As for "writing code" itself? Its weight is genuinely declining. Not that it's unnecessary — it's just **no longer the bottleneck**.

## One line

**In the AI era, the point of technical knowledge isn't to write code yourself — it's to see what's wrong with the code AI writes.**

If you can see it, you have leverage: one judgment call, and AI implements it across dozens of files. If you can't, you're relying on a tool you can't verify — and it will confidently tell you everything is fine.

---

*These observations come from refactoring [jsbench](https://github.com/hongzhidao/jsbench), an HTTP benchmarking tool originally written by AI. The full technical series documenting the refactoring process is [here](https://github.com/hongzhidao/jsbench/tree/main/docs) (6 articles, ongoing).*
