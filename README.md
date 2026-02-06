# jsbench

A high-performance HTTP/HTTPS benchmarking tool powered by **C + QuickJS + epoll**.

Write benchmarks in JavaScript, execute at native speed.

## Why jsbench?

Tools like `wrk` and `ab` are fast but limited — custom scenarios require Lua or aren't possible at all. `k6` and `autocannon` are scriptable but sacrifice raw throughput.

**jsbench** gives you both:

- **C-level throughput** (140K+ QPS) for simple benchmarks — no JS overhead in the hot path
- **JavaScript flexibility** with a standard `fetch()` API for complex scenarios (auth flows, chained requests, dynamic payloads)
- **Zero runtime dependencies** — single binary, QuickJS embedded, just needs OpenSSL

## Features

- **140K+ QPS** on a single machine (nginx benchmark, aarch64)
- **Four benchmark modes**: URL string, request object, array round-robin, async function
- **HTTP keep-alive** connection reuse for maximum throughput
- **Web-standard `fetch()` API** with `Response`, `Headers`, `.json()`, `.text()`
- **Multi-threaded**: epoll per worker, connections distributed across threads
- **Two-tier latency histogram**: 0-10ms at 1us resolution, 10ms-1s at 100us resolution
- **TLS/HTTPS** support via OpenSSL with SNI
- **CLI mode**: run scripts with top-level `await` for quick HTTP testing

## Quick Start

```bash
git clone https://github.com/hongzhidao/jsbench.git
cd jsbench
make        # downloads and builds QuickJS automatically
./jsb bench.js
```

### Requirements

- Linux (epoll-based)
- GCC or Clang
- OpenSSL development headers (`libssl-dev` on Debian/Ubuntu, `openssl-devel` on RHEL/Fedora)
- Git (for fetching QuickJS)

## Usage

```bash
# Benchmark mode - script has export default
./jsb bench.js

# CLI mode - script has no default export
./jsb test.js
```

## Script Format

A benchmark script is an ES module with two exports:

- **`export default`** - what to request (required for benchmark mode)
- **`export const bench`** - how to run (optional)

### `default` export

| Type             | Path   | Description                              |
|------------------|--------|------------------------------------------|
| `string`         | C      | URL -> GET request                       |
| `object`         | C      | `{ url, method, headers, body }`         |
| `array`          | C      | Array of the above, round-robin          |
| `async function` | JS     | Custom scenario with `fetch()` calls     |

String/object/array exports use a **pure C hot path** - no JavaScript in the benchmark loop. Async function exports run a **per-thread QuickJS runtime**.

### `bench` export

| Property      | Default | Description                                |
|---------------|---------|--------------------------------------------|
| `connections` | `1`     | Number of concurrent connections           |
| `duration`    | `10s`   | Benchmark duration (e.g. `'10s'`, `'1m'`)  |
| `threads`     | `1`     | Number of worker threads                   |
| `target`      | -       | Override base URL                          |
| `host`        | -       | Override HTTP `Host` header                |

## Examples

### Simple GET

```js
export const bench = {
    connections: 100,
    duration: '10s',
    threads: 4
};

export default 'http://localhost:8080/';
```

### POST with JSON body

```js
export const bench = {
    connections: 100,
    duration: '10s'
};

export default {
    url: 'http://localhost:8080/api/users',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: 'test' })
};
```

### Multiple endpoints (round-robin)

```js
export const bench = {
    connections: 100,
    duration: '10s'
};

export default [
    'http://localhost:8080/health',
    {
        url: 'http://localhost:8080/api/users',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: 'test' })
    }
];
```

### Custom scenario (login -> use token)

```js
export const bench = {
    connections: 50,
    duration: '10s',
    target: 'http://localhost:8080'
};

export default async function() {
    const res = await fetch('http://localhost:8080/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ user: 'admin', pass: 'secret' })
    });
    const { token } = await res.json();

    return await fetch('http://localhost:8080/api/profile', {
        headers: { 'Authorization': `Bearer ${token}` }
    });
}
```

### CLI mode

Scripts without `export default` run as plain scripts with top-level `await`:

```js
var resp = await fetch('http://localhost:8080/api/health');
console.log('status:', resp.status);

var data = await resp.json();
console.log('body:', data);
```

## Runtime API

### `await fetch(url[, options])`

| Option    | Description                |
|-----------|----------------------------|
| `method`  | HTTP method (default `GET`)|
| `headers` | Request headers object     |
| `body`    | Request body string        |

### `Response`

| Property / Method | Type    | Description        |
|-------------------|---------|--------------------|
| `.status`         | number  | HTTP status code   |
| `.statusText`     | string  | Status text        |
| `.ok`             | boolean | `status` is 2xx    |
| `.headers`        | Headers | Response headers   |
| `await .text()`   | string  | Body as string     |
| `await .json()`   | object  | Parsed JSON body   |

### `Headers`

`.get(name)` / `.has(name)` / `.set(name, value)` / `.delete(name)` / `.forEach(cb)`

### `console.log(...args)`

Print to stdout.

## Output

```
Running benchmark: 100 connection(s), 4 thread(s), 10.0s duration
Target: http://localhost:8080/health

  requests:  1423456
  duration:  10.00s
  bytes:     227.5 MB
  errors:    0
  qps:       142345.6

  latency    min       avg       max       stdev
             0.02ms    0.68ms    5.12ms    0.31ms

  percentile p50       p90       p99       p999
             0.58ms    1.10ms    2.34ms    4.50ms

  status     2xx       3xx       4xx       5xx
             1423456   0         0         0
```

## How it was built

*"Make it work, make it right, make it fast."* — Kent Beck

The first version of jsbench was built entirely through AI-assisted development — the human provided architecture decisions and requirements, the AI (Claude Opus 4.6 via [Claude Code](https://claude.ai/claude-code)) wrote all the code. AI is already very good at "make it work", but the design isn't where I want it yet. I'm now manually refactoring the internals to make it right.

This project is actively maintained. Issues and pull requests are welcome.

## License

MIT
