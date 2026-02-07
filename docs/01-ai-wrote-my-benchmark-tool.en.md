# jsbench: A Programmable Benchmark Tool Written by AI in Half a Day

If you do backend work, you've used benchmarking tools. But have you noticed the awkward state of things?

wrk — top-tier performance, but scripting is in Lua. Want to do a "login first, grab a token, then hit the API" scenario? Good luck.

k6 — JavaScript scripting, full-featured. But it's a ~60MB install, and you need to learn a custom API — `http.get()`, `check()`, `sleep()`. I just want to hit one endpoint, not learn a framework.

What I wanted was simple: wrk's performance, JS flexibility, zero learning curve.

So I built one: **jsbench**.

## Benchmark an Endpoint in Three Lines

```js
export default 'http://localhost:3000/api';

export const bench = { connections: 100, duration: '10s', threads: 4 };
```

```bash
jsb bench.js
```

That's it.

Need POST? Change the export default:

```js
export default {
    url: 'http://localhost:3000/api/users',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: 'test' })
};
```

Need to login, grab a token, then hit the API? Use an async function:

```js
export default async function() {
    const res = await fetch('http://localhost:3000/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ user: 'admin', pass: 'secret' })
    });
    const { token } = await res.json();

    return await fetch('http://localhost:3000/api/profile', {
        headers: { 'Authorization': `Bearer ${token}` }
    });
}
```

Notice: no new APIs. `fetch`, `export default`, `async/await` — you already know all of this.

## Under the Hood: C + QuickJS

jsbench is written in C, embedding Fabrice Bellard's QuickJS engine.

JS scripts run inside QuickJS to produce request configurations, which are then handed off to a C-level epoll event loop. The networking layer is pure C non-blocking I/O with HTTPS/TLS support, each worker thread running its own epoll instance.

The build output is a single statically-linked binary with no runtime dependencies. Just grab it and go.

## Performance: Same League as wrk

Let's address what everyone cares about first. jsbench's networking layer is pure C — epoll event loop, non-blocking I/O, each thread running its own instance. Same architecture as wrk. JS only generates the request configuration; the actual load is driven by C. So for static benchmarks (fixed URL, fixed body), jsbench's QPS is in the same league as wrk.

Not "close to" — the same league. Because the bottleneck is the network and kernel, not the tool itself.

## Comparison with wrk and k6

|                 | wrk         | k6               | jsbench             |
|-----------------|-------------|-------------------|---------------------|
| Language        | C           | Go                | C                   |
| Scripting       | Lua (limited) | JS (custom API) | JS (standard fetch) |
| Performance     | Top-tier    | Medium            | Top-tier (same arch)|
| Binary size     | Small       | Large (~60MB)     | Small               |
| Complex scenarios | Difficult | Supported        | Supported           |
| Learning curve  | Low         | Medium            | Zero                |

jsbench isn't trying to replace k6 — k6 has cloud services, dashboards, a full ecosystem. jsbench's niche is: wrk-level performance, JS flexibility, for when you just want to quickly hit something and a JS file is all you need.

## Some Honest Thoughts on AI-Assisted Programming

This project went from zero to functional in one day, mostly written by AI. The tool was Claude Code + Opus.

But I don't want to write a hype piece about how amazing AI is. Let me say upfront: **many people are better at AI-assisted programming than I am.** You can find impressive examples everywhere — complete products built in hours, polished UIs generated from prompts, complex systems assembled from scratch. Compared to them, my approach is plain. I'm just a backend developer with over a decade of C experience who used AI to build a small tool in my own domain. So what follows is personal experience, not the ceiling of what AI can do.

### My Approach

I didn't do much. I thought through the requirements, wrote them up as a README, and defined the API and script format. I also prepared architecture docs as context for the AI — though those docs were themselves AI-generated. The only real "intervention" was telling it the file naming conventions — a habit I picked up on the nginx team. Beyond that, I let the AI write everything.

One thing worth noting: **having AI write the test cases is currently the best way to ensure AI code quality.** You don't need to review every line — just run the tests and you'll know if it works.

### Can AI Actually Write Code?

The good parts first. Naming — a notoriously hard problem in programming. But for an AI that can handle natural language, giving variables and functions accurate names is actually one of its easiest tasks. This part you can safely delegate.

Logic is decent too. But good code strives for simplicity and clarity, and AI still falls short there.

I have a feeling: **AI's aesthetic sense is different from ours.** Humans pursue simplicity partly because our brains have limited capacity — we've had to develop ways of understanding that work within our cognitive constraints. AI has near-brute-force compute; it doesn't need that compromise. Like AI in chess — its moves no longer follow human logic, but it wins.

I believe AI will develop its own programming style — rigorous, effective, but not necessarily matching human aesthetics.

### Where AI Is Weakest: Architecture

How to organize code, how to split modules, where to put abstractions — this is AI's most obvious weakness right now. I've been using AI for programming since the early days, and every version improves, but this area still doesn't satisfy me.

There's an emerging approach: two AIs working simultaneously, one writing code and one reviewing, creating an adversarial loop to improve quality. I didn't do that for this project — I wanted to see what AI-written code looks like on its own.

I'll continue maintaining this project and refactoring the architecture myself. **What will be different between human-written and AI-written code?** Follow along if you're curious.

### AI-Assisted Programming Is a Real Trend, but the Impact Will Lag

One thing I'm fairly certain of: AI-assisted programming isn't hype — it's a real trend. But technology trends have a pattern — **the real impact often comes with a delay.** When the internet first appeared, everyone thought it would change everything, then the bubble burst and many said it was overblown. Ten years later, it really did change everything.

AI-assisted programming might be in that phase now. The tools improve every month, but truly changing how software is built may take more time to settle. What will the final form look like? What will a programmer's role become? Honestly, I don't know either, but I'm curious.

## Open Source

GitHub: https://github.com/hongzhidao/jsbench

Stars and issues welcome.

Oh, and jsbench isn't just a benchmarking tool — it's also its own test tool. The entire test suite runs on jsbench itself. Testing yourself with yourself — that's probably the most honest form of dogfooding.

Also, this article was largely AI-generated too. I provided the opinions; the AI did the writing. So yes, even the part criticizing AI was written by AI.
