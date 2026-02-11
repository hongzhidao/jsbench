I've been writing code for over a decade. I work on the nginx team. Recently I had AI (Claude Code + Opus 4.6) write a programmable HTTP benchmarking tool from scratch — C + QuickJS, ~2,000 lines, running in a day. Then I started refactoring its architecture, one commit at a time.

What I learned doesn't match either camp — not "AI will replace us all" nor "it's just hype."

**AI's most dangerous bugs are invisible.** jsbench lets users write JS scripts calling fetch() for load testing. AI wrote the feature and the tests. Report: 16,576 requests, 0 errors. PASS. But every single fetch had failed. The worker thread had no event loop — fetch() couldn't send anything. The code just unconditionally counted each call as success. AI-written code and AI-written tests shared the same blind spot. Not crashes — programs that run fine, pass all tests, and produce wrong results.

**With the right direction, AI is your entire team.** fetch() didn't support concurrency — Promise.all with three requests took 900ms instead of 300ms. AI had implemented "fake async": Promise signature, synchronous blocking inside. I knew the fix: register with a global event loop, return a pending Promise, let the loop drive I/O. I gave AI the problem, the architecture, the existing code, and the constraints. It got it right in one shot — 9 files, 905ms → 302ms. If I'd just said "fetch has a bug," it would've patched around the broken architecture. But a clear direction got a correct structural change.

**Judgment is the real multiplier.** I combined epoll and timers into one "engine" object — one per thread. Simple idea, but 6 files, 20+ call sites. AI changed every one without missing any. If the judgment had been wrong, AI would've applied the mistake just as thoroughly. One architectural call, applied across dozens of files — enormous leverage either way.

**What becomes more valuable:** Architectural judgment — AI implements any direction but won't choose one. Code review — AI produces bugs as fast as code; spotting logic/architecture problems is now a defensive necessity. Domain depth — I knew fetch() needed an event loop because I've written event-driven systems for a decade, not because of a good prompt. AI amplifies abilities you already have; it doesn't create them.

**One line:** In the AI era, technical knowledge isn't for writing code — it's for seeing what's wrong with AI's code. See it, and you have leverage. Miss it, and you're trusting a tool that will confidently tell you everything is fine.

Full series (ongoing): https://github.com/hongzhidao/jsbench/tree/main/docs
