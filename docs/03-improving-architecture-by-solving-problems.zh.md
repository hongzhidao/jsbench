# AI 写的代码全部通过测试，但每个请求都失败了

> 好的架构不是设计出来的，是在解决问题的过程中长出来的。

上一篇说到代码组织——文件命名、前缀风格、构建分离。那些是架构的骨架。但骨架搭好之后，真正考验架构的，是它能不能支撑住功能的演进。

这一篇，我用一个具体的问题来讲架构改进：**fetch() 不支持并发**。

## 问题

jsbench 的卖点之一是标准的 `fetch()` API。用户可以在 JS 里用 `await fetch()` 发请求，跟浏览器里写法一样。

但我发现了一个问题：`Promise.all` 不 work。

```js
// 理论上 3 个请求并发，总耗时应该 ≈ 300ms
const responses = await Promise.all([
    fetch('http://localhost/delay/300'),
    fetch('http://localhost/delay/300'),
    fetch('http://localhost/delay/300'),
]);
// 实际耗时：~900ms — 串行执行，没有并发
```

我写了个测试来验证：

```js
// 3 个请求，每个 300ms 延迟
// 如果并发：总耗时 ~300ms
// 如果串行：总耗时 ~900ms

var t0 = Date.now();
var responses = await Promise.all(urls.map(url => fetch(url)));
var elapsed = Date.now() - t0;

// 结果：elapsed ≈ 900ms
// CONCLUSION: fetch() is BLOCKING - no real concurrency
```

3 个 `Promise.all` 跟 3 个顺序 `await` 耗时一样。fetch 是阻塞的，没有真正的并发。

## 根因

看一眼 `js_fetch.c` 的实现就明白了：

```c
static JSValue js_fetch(JSContext *ctx, ...) {
    // 1. 阻塞式 DNS 解析
    int gai_err = getaddrinfo(url.host, url.port_str, &hints, &res);

    // 2. 创建连接
    js_conn_t *conn = js_conn_create(...);

    // 3. 创建 **本地** epoll 实例
    int epfd = js_epoll_create();

    // 4. 同步等待，直到响应完成
    while (!done) {
        int n = epoll_wait(epfd, events, 4, 1000);
        // ... 处理事件 ...
    }

    // 5. 返回已经 resolve 的 Promise
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
    JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 1, &result);
    return promise;  // ← 已经 resolve 了，不是 pending
}
```

问题出在三个地方：

**DNS 解析阻塞。** `getaddrinfo()` 是同步调用，会阻塞整个 JS 运行时。

**每次 fetch 创建独立的 epoll。** 不是挂到全局事件循环上，而是自己建一个，跑完就销毁。

**Promise 立刻 resolve。** 函数返回的时候 I/O 已经做完了，Promise 只是个空壳——包装了一个已完成的结果。

根本原因：AI 用了一种"伪异步"的实现方式——函数签名看起来是异步的（返回 Promise），但内部是完全同步阻塞的。接口是 Web Fetch API，实现是同步 HTTP client。

## 我知道正确的架构是什么

作为写了多年 nginx 的人，我很清楚这类问题该怎么解决。正确的做法是：

1. fetch() 只负责创建连接、注册到全局事件循环，然后返回一个 **pending** 的 Promise
2. 全局事件循环统一管理所有连接的 I/O
3. 当某个连接的响应回来了，resolve 对应的 Promise
4. DNS 解析也需要异步化

这就是 nginx 的模型，也是所有高性能异步 I/O 框架的标准模式——事件驱动、非阻塞、统一调度。

其实 jsbench 的 C-path（纯性能路径）已经是这个架构了。worker 线程的 epoll 实例同时管理上百个连接，状态机驱动每个连接的生命周期。代码里甚至已经有 `js_loop.c`，设计了 pending fetch 的管理结构。

只是 JS-path 的 fetch() 没有接入这个体系。AI 走了一条捷径——同步阻塞，包一层 Promise。能用，但不对。

## 但说实话，手动改并不轻松

虽然我知道怎么改，但客观地说，如果全部手动调整，难度和工作量还真不少：

- fetch() 要从同步阻塞改成异步非阻塞，返回 pending Promise
- 需要一个全局事件循环来管理多个并发连接
- Promise 的 resolve/reject 要跟事件循环对接
- QuickJS 的 job queue 要能在事件循环中正确驱动
- DNS 解析要异步化，不能阻塞事件循环
- 错误处理、超时、TLS 握手都要适配新模型

这不是改几行代码的事。这是一次架构级的改动——把 fetch 从一个同步函数变成一个真正的异步操作，牵涉到事件循环、Promise 机制、连接管理的全链路。

## 先让 AI 来

所以我的做法是：**先把问题和方向讲清楚，让 AI 来修**。

这听起来有点"偷懒"，但其实这正是 AI 时代架构改进的一种方式。上一篇我说过——架构对了，AI 就是你的整个团队。反过来说，当你有清晰的架构方向时，把实现交给 AI 是完全合理的。

关键是你要给 AI 讲清楚这几件事：

1. **问题是什么**：fetch() 当前是阻塞的，Promise.all 无法实现并发
2. **正确的方向是什么**：fetch 应该返回 pending Promise，I/O 由全局事件循环统一驱动
3. **已有的基础是什么**：C-path 已经有完整的事件循环和连接管理，js_loop.c 有现成的结构
4. **约束是什么**：要跟 QuickJS 的 job queue 正确对接，保持 Web Fetch API 的语义

你不需要告诉 AI 每一行怎么写。你给它问题、方向、上下文和约束，它来做实现。

这说明一个事情：**AI 是能改进架构的，前提是你把问题讲清楚。** 反过来说，如果在写代码之前就把架构讲清楚，AI 也能实现得不错。但"讲清楚"本身取决于经验——你得知道什么是对的，才能告诉 AI 往哪个方向走。

我相信 AI 的架构能力会越来越好。第一篇我说过架构是 AI 最明显的短板，但短板不代表永远是短板。就像 AI 下棋，一开始也不如人类，后来超过了所有人。架构这件事，本质上也是模式识别加经验积累，AI 没有理由做不好。

## 结果

AI 改完之后，跑测试：

```
Promise.all with 3 x 300ms delay:
  Total elapsed: 302ms          ← 之前是 905ms
  RESULT: CONCURRENT (fetches ran in parallel)
```

905ms → 302ms。`Promise.all` 真正并发了。

改动涉及 9 个文件，核心变化：

- **js_fetch.c**：去掉了阻塞式 epoll 循环，fetch() 改为注册连接到全局事件循环，返回 pending Promise
- **js_loop.c**（新增）：集中式事件循环，一个 epoll 管理所有连接，交替驱动 I/O 事件和 QuickJS job queue
- **js_cli.c**：原来 34 行的执行逻辑缩减到 5 行，委托给 `js_loop_run()`
- **js_event_loop.c → js_epoll.c**：重命名，因为它本来就只是 epoll 的封装，不是真正的事件循环

从结果看，AI 做得不错。给它讲清楚问题和方向之后，它一次就把架构改对了。

## 怎样改进架构

回头看这次改动，我觉得路径其实很清晰：

**1. 先发现问题。** 改架构的前提是你知道哪里有问题。发现问题通常有两条路：review 代码，或者写测试用例覆盖到。这次两条路都用了——我看了 `js_fetch.c` 的实现，发现它是同步阻塞的；同时写了并发测试，用 905ms vs 300ms 把问题量化了。没发现问题，就没有改进的起点。

**2. 让 AI 先修。** 发现问题之后，不用急着自己动手。把问题描述清楚，AI 往往能修复它——而修复问题的过程本身就会带来架构改进。这次 AI 把 fetch 从同步阻塞改成了异步事件循环，这不只是修了一个 bug，而是架构往正确的方向走了一步。

**3. 搞清楚正确的方向。** 知道问题在哪还不够，你得知道对的做法是什么。这次是全局事件循环 + pending Promise，这个判断来自经验。经验不够的时候，可以看成熟项目怎么做——nginx、libuv、Node.js，都是同一个模型。

**4. 给 AI 讲清楚，让它来实现。** 问题、方向、已有基础、约束——讲清楚这四样，AI 就能做出正确的实现。这次 AI 一次就改对了，9 个文件，905ms → 302ms。

**5. 用测试验证。** 改之前写好测试，改完跑一遍。302ms 就是这次的验证标准。

这个路径不只适用于这一次。任何架构改进，归根到底都是这几步：发现问题、想清楚方向、实现、验证。区别只在于"想清楚方向"这一步——这取决于经验，也是目前人比 AI 更有优势的地方。

代码变更: [1179d57](https://github.com/hongzhidao/jsbench/commit/1179d57)

## 但故事没有到此结束

AI 改完了，`make test` 跑一遍，14 个测试全部通过。看起来没问题了。

但真的没问题吗？

fetch() 的实现改了——从同步阻塞变成了异步事件循环。那 benchmark 的 async function 模式呢？它也调用 fetch()，还能正常工作吗？

看一眼 `js_worker.c`：

```c
static void worker_js_path(js_worker_t *w) {
    JSRuntime *rt = js_vm_rt_create();
    JSContext *ctx = js_vm_ctx_create(rt);   // ← 新的 context，没有 event loop

    while (!stop) {
        JSValue promise = JS_Call(ctx, default_export, ...);

        while (JS_ExecutePendingJob(rt, &pctx) > 0);

        w->stats.requests++;       // ← 无条件 +1
        w->stats.status_2xx++;     // ← 无条件算成功
    }
}
```

Worker 创建了新的 JS context，但没有设置 event loop。fetch() 找不到 loop，直接报错。**每一次 fetch 都失败了。**

但测试报告：

```
Async function                       PASS (16576 reqs, 0 errors)
```

16576 次请求，0 错误。**实际上每次都失败了，但报告说全部成功。**

原因是程序把失败算成了成功——worker 没有检查 fetch 是否真的拿到了响应，无条件计入成功。测试只看了这个数字，自然发现不了问题。

但这不是我们改 fetch 引起的。**这段代码从一开始就有问题。** 改之前 fetch 是同步阻塞的，确实能返回响应。但 worker 从来没有检查过响应结果，测试也没有验证实际结果。只是以前碰巧能跑通，问题被掩盖了。

**AI 写的代码"能跑"，AI 写的测试"能过"，但两者都没有验证真正重要的东西。** 这是 AI 生成代码的一个典型陷阱：实现和测试是同一个 AI 写的，它们有一致的盲区。实现跳过了错误检查，测试也跳过了错误验证——它们完美地互相配合，形成了一个看起来一切正常的假象。

我是怎么发现的？Review 代码。我清楚每个 worker 线程都应该有自己的事件引擎——epoll_wait 来驱动 I/O。CLI 模式有，C-path 有，但 bench function 模式竟然没有。

发现问题有两条路：review 代码和写测试。这篇文章里第一个问题（fetch 不支持并发）就是测试发现的——905ms vs 300ms，一目了然。但这个问题测试没发现，因为 AI 写的测试和 AI 写的实现有一致的盲区：实现不检查错误，测试也不验证错误，两者互相"配合"，制造了一切正常的假象。这种时候就得靠人 review。

发现问题之后，AI 修复非常快。给 worker 创建 event loop，用 `js_loop_run()` 驱动 I/O，根据返回值判断成功还是失败——讲清楚之后，AI 几分钟就改好了。bench async 模式真正能跑了。

现阶段的 AI 会引入严重的 bug——不是偶尔，是经常。而且它引入的 bug 往往不是显式的崩溃，而是像这次一样：程序能跑，测试能过，但结果是错的。这种 bug 最危险，因为你不知道它在那里。

所以 review 代码仍然是最有效的质量保障方式。这不是新道理。我在 nginx unit、njs 和 nginx 团队都呆过，这几个项目的代码 bug 都比较少，除了开发人员水平高以外，review 是必须做的一件事。每次提交都有人仔细看，有时候 reviewer 花的时间比写代码的人还多。这个流程在 AI 时代不但没有过时，反而更重要了——因为 AI 写代码的速度远快于人类，如果没有 review，bug 堆积的速度也会远快于人类。

当然，用另一个 AI 来 review 也能改善这个问题。两个 AI 对抗——一个写代码，一个挑毛病——比一个 AI 自己写自己测要靠谱。随着 AI 水平越来越高，相信对于很多项目，这类问题会少很多。

但短期看，人为的 review 还是有必要的。而且这跟项目性质有关。有些项目求快——工具类、脚本类，写完能用就行，出了问题改就是了。但有些项目不一样，比如服务端程序，要经历各种极端情况的考验：高并发、异常断连、内存泄漏、超时边界。这些 corner case 不会在 happy path 的测试里暴露，需要有经验的人去审视。

所以懂架构看起来还是需要的。不是说要你手写每一行代码，而是你得能看出哪里不对。AI 能写代码、能修 bug、能改架构，但前提是有人告诉它哪里有问题。如果你自己看不出来，AI 也不会主动告诉你。

代码变更: [5e6e438](https://github.com/hongzhidao/jsbench/commit/5e6e438)

## 小结

这篇文章做了两件事：修了 fetch 的并发问题，顺带发现并修了 bench async 模式的统计 bug。两次改动都推动了架构往正确的方向走。

回头看，几个观察：

- 解决问题和增加需求，都是改进架构的好时机
- AI 能改进架构，前提是你把问题讲清楚
- AI 写的代码和 AI 写的测试有一致的盲区，review 仍然是最有效的质量保障
- 懂架构不是为了手写代码，而是为了看出哪里不对

到目前为止，我们做的还是修修补补——修一个问题，改进一点架构。下一篇想聊聊：到底什么是好的架构？

---

GitHub: https://github.com/hongzhidao/jsbench
