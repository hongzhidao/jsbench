# Refactoring AI-Generated C: An nginx Engineer's Standards

> "Make it work, make it right, make it fast." — Kent Beck

In the previous post, I mentioned that the first version of jsbench was mostly written by AI. It runs, the tests pass. "Make it work" — AI nailed that.

But "make it right"? That's what I'm doing now — refactoring the architecture.

When I started this project, I had two choices: lay out the architecture upfront and have AI follow my standards, or let AI generate a working version freely and refactor it myself afterward.

I chose the latter. The reason is simple: **I wanted to see what AI produces on its own.** What does it do well? Where does it fall short? You don't really know until you look. This is a form of learning in itself. The refactoring process is essentially a conversation with AI-generated code.

## Why Architecture

I said before that architecture is AI's most obvious weakness. But flip that around and it reveals something important: **if the architecture is right, the remaining business code is easy for AI.**

Think about it. AI can already write correct functions, reasonable names, and complete tests. What it lacks is how to organize code, how to split modules, where to put abstractions.

This means architecture has become more valuable in the AI era. Before, good architecture affected team collaboration and maintenance costs. Now, good architecture directly determines whether you can use AI effectively to write code.

**Get the architecture right, and AI becomes your entire team. Get it wrong, and every line AI writes is technical debt.**

So the main thread of this series is refactoring the architecture. One change at a time, each with a clear rationale.

## First Cut: Code Organization

The first step of architecture isn't some sophisticated design pattern — it's the basics. How files are named, how code is organized, where build artifacts go.

These decisions seem trivial, but they define the project's skeleton. Everything that follows — module boundaries, dependency management, interface design — builds on top of this.

### AI Actually Did Well

Let me be fair. The AI-generated code had a good habit: every function and type used a `jsb_` prefix.

```c
jsb_conn_t *jsb_conn_create(...);
void jsb_conn_free(jsb_conn_t *c);
int jsb_http_response_feed(jsb_http_response_t *r, ...);
```

C has no namespaces, so prefixes to avoid collisions are standard practice. AI knew this and executed it consistently — 13 source files, over a hundred functions and types, not a single prefix missed. That kind of discipline is honestly better than many human projects.

File naming was also reasonable: `http_client.c`, `http_parser.c`, `event_loop.c` — self-explanatory.

So it's not that AI did poorly. It's that I have my own standards — shaped by years of writing C on the nginx team.

### Change 1: Code Prefix `jsb_` → `js_`

`jsb` stands for "jsbench" — a perfectly reasonable choice by the AI. But I prefer `js_` — shorter, and more aligned with the project's core identity (a JS runtime). In the nginx ecosystem, prefixes are always short project abbreviations: `ngx_`, `njs_`.

```c
// before
jsb_conn_t *jsb_conn_create(...);

// after
js_conn_t *js_conn_create(...);
```

### Change 2: File Names with `js_` Prefix

```
// before
src/main.c
src/http_client.c
src/http_parser.c
src/jsb.h

// after
src/js_main.c
src/js_http_client.c
src/js_http_parser.c
src/js_main.h
```

In nginx and njs, every file carries the project prefix. The benefit is practical: when searching for files in a large working directory, the prefix lets you quickly identify which project a file belongs to.

### Change 3: Separating Build Artifacts

AI generated `.o` files directly in `src/` — source code mixed with build artifacts. I moved them to a `build/` directory, keeping `src/` purely for source code.

```
// before
src/main.c
src/main.o    ← mixed together

// after
src/js_main.c
build/js_main.o    ← separated
```

## Code Is for Humans

These changes don't affect functionality or performance. All 14 tests still pass.

But code isn't just for machines to execute — **code is for humans to read**. Naming conventions, file organization, directory structure — all of these reduce the cognitive cost of understanding code.

AI doesn't need any of this. `a.c`, `b.c`, `x_func_1` — makes no difference to AI. But it makes a difference to humans.

In the previous post, I said AI's aesthetic sense differs from ours. This is a concrete example. AI's choices weren't wrong, but I have my own standards — from years of writing C on the nginx team.

That said, I believe AI will eventually handle naming and code organization very well. It's not a deep intelligence problem — it's more about aligning with project conventions and personal preferences. Give AI enough context — "follow nginx naming conventions" — and it would likely generate code that matches your expectations. This time I didn't provide that context, so it used its own judgment. The judgment wasn't bad — just not exactly what I wanted.

## What's Next

The skeleton is in order. Next up: real module design.

---

GitHub: https://github.com/hongzhidao/jsbench

Code changes for this article: [559a644](https://github.com/hongzhidao/jsbench/commit/559a644)
