# 封装复杂性：一个反复生效的架构手法

架构的手法不需要很多。真正有用的，一个就够——反复用。

这一篇做一件事：把 fetch 的复杂度从事件循环里搬回 fetch 自己手里。拆一个 529 行的文件，搬一堆不该在 loop 里的逻辑，切断最后的类型依赖。结果：loop 从 224 行变成 85 行，功能一行没少。

这不是第一次用这个手法。第五篇用它封装了事件引擎，调用者从写二十行变成写一行。这一篇用同样的手法封装 fetch。**如果同一个手法在不同的地方都能生效，它就不是技巧，是原则。**

## Loop 在做谁的活

先看问题。

`js_loop.c` 是事件循环——一个调度器。但翻开代码，224 行里超过一半在做别人的事：

```c
static void loop_on_read(js_event_t *ev) {
    // ... 读数据、喂 HTTP 解析器、判断完成状态 ...
}

static void pending_complete(js_loop_t *loop, js_pending_t *p) {
    // ... 构建 Response、resolve Promise、清理资源 ...
}

static void pending_fail(js_loop_t *loop, js_pending_t *p, const char *msg) {
    // ... reject Promise、清理资源 ...
}
```

HTTP 解析、Promise resolve/reject、连接清理——这些全是 fetch 的活。**一个调度器，在做 HTTP 客户端的事。**

再看 `js_fetch.c`。529 行，但不是 fetch 太复杂——是三个完全不同的概念挤在一起：Headers 类的完整实现、Response 类的完整实现、fetch 函数。三者各有各的类型定义和初始化逻辑，但共享一个文件作用域，没有任何隔离。

两个问题，同一个根因：**复杂度不在它该在的地方。**

## 第一步：拆文件——让每个概念有边界

529 行里，Headers 和 Response 各自是独立的 JS 类——类型定义、方法、注册，自成一体。fetch 只在最后一步创建 Response。三者之间的依赖很弱。

拆成三个文件：

- `js_headers.c`（181 行）：Headers 的全部实现。核心类型 `js_headers_t` 是文件内部的——离开这个文件，没人知道它长什么样
- `js_response.c`（142 行）：Response 的全部实现。`js_response_t` 同样只在文件内可见
- `js_fetch.c`：只剩 fetch 的核心逻辑

对外接口收拢到 `js_fetch.h`——五行声明：

```c
void    js_headers_init(JSContext *ctx);
JSValue js_headers_from_http(JSContext *ctx, const js_http_response_t *parsed);
void    js_response_init(JSContext *ctx);
```

C 没有 class 的 private 关键字，但文件作用域就是天然的封装边界。**文件本身就是模块。** 这个道理不限于 C——Java 和 Go 用包，Python 用模块，TypeScript 用文件导出。形式不同，本质一样：给概念一个边界，让内部细节不泄漏。

代码变更: [c3181c1](https://github.com/hongzhidao/jsbench/commit/c3181c1)

## 第二步：搬行为——让 fetch 管好自己的事

结构拆干净了，但行为还散着。loop 里的 `loop_on_read`、`loop_on_write`、`pending_complete`、`pending_fail`——全是 fetch 该做的事。

搬回去。

先统一清理路径。之前 complete 和 fail 各有一套资源释放代码，大部分重复。引入 `js_fetch_destroy()`，一个函数管所有路径：

```c
void js_fetch_destroy(js_fetch_t *f) {
    js_pending_t *p = &f->pending;
    JSContext *ctx = p->ctx;

    js_epoll_del(js_thread()->engine, &f->conn->socket);
    js_timer_delete(&js_thread()->engine->timers, &f->timer);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    js_http_response_free(&f->response);
    js_conn_free(f->conn);
    if (p->ssl_ctx) SSL_CTX_free(p->ssl_ctx);
    list_del(&p->link);
    free(f);
}
```

然后 complete 和 fail 变成清晰的两步——先做自己的事，再调 destroy：

```c
static void js_fetch_complete(js_fetch_t *f) {
    JSValue response = js_response_new(ctx, ...);
    JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, &response);
    js_fetch_destroy(f);
}

static void js_fetch_fail(js_fetch_t *f, const char *message) {
    JSValue err = JS_NewError(ctx);
    JS_Call(ctx, p->reject, JS_UNDEFINED, 1, &err);
    js_fetch_destroy(f);
}
```

超时处理从之前的十几行变成一行：

```c
static void js_fetch_timeout_handler(js_timer_t *timer, void *data) {
    js_pending_t *p = data;
    js_fetch_fail(js_fetch_from_pending(p), "Request timeout");
}
```

事件处理函数也搬过来。回调在 `js_fetch()` 创建连接时就绑定好：

```c
conn->socket.read  = js_fetch_on_read;
conn->socket.write = js_fetch_on_write;
conn->socket.error = js_fetch_on_error;
```

**创建者就是管理者。** 谁创建了连接，谁就负责它的事件处理和生命周期。这个原则在任何语言里都成立——React 里谁创建了 state 谁管理它，Go 里谁启动了 goroutine 谁负责关闭它。

代码变更: [eb6a070](https://github.com/hongzhidao/jsbench/commit/eb6a070)

## 第三步：切断最后的依赖

行为搬回去了，但 loop 还知道 `js_fetch_t`——`loop_free` 调 `js_fetch_destroy(js_fetch_from_pending(p))`，`loop_add` 做 `js_epoll_add`。Loop 的代码里还有 fetch 的影子。

怎么让 loop 彻底不知道 fetch？答案是函数指针——C 语言的多态。

给 `js_pending_t` 加一个 `destroy` 回调：

```c
struct js_pending {
    /* ... */
    void (*destroy)(js_pending_t *p);
};
```

fetch 在创建时注册自己的销毁函数：

```c
p->destroy = js_fetch_destroy;
```

loop 清理时只管调回调，不管对面是谁：

```c
// before: loop 知道 fetch
js_fetch_destroy(js_fetch_from_pending(p));

// after: loop 只知道 pending
p->destroy(p);
```

同时，`js_fetch_t` 从 `js_main.h` 搬进 `js_fetch.c`，变成文件私有类型。epoll 注册也从 `loop_add` 搬回 `js_fetch()`。

至此，loop 里没有任何 fetch 相关的类型、函数、宏。**最后一根线切断了。**

这个模式在面向对象语言里叫接口或抽象类——Go 的 `io.Closer`，Java 的 `AutoCloseable`。C 里没有这些语法糖，但函数指针做的是同一件事：**调用者不需要知道具体类型，只需要知道它能做什么。** Loop 不需要知道 pending 操作是 fetch 还是 WebSocket，只需要知道它有一个 `destroy`。

代码变更: [88045f2](https://github.com/hongzhidao/jsbench/commit/88045f2)

## 结果

三步做完，`js_loop.c` 变成了 85 行：

```
js_loop_create()   →  创建 pending 列表
js_loop_free()     →  遍历 pending 调 p->destroy(p)
js_loop_add()      →  加入列表
js_loop_run()      →  驱动 JS job queue → 检查 pending → epoll poll → 触发定时器
```

没有 HTTP 解析，没有 Promise 操作，没有连接状态判断，**没有任何 fetch 相关的类型**。纯粹的调度。如果将来要支持 WebSocket，loop 一行不用改——新协议实现自己的 `destroy` 回调，loop 照样调度。

对比一下前后：

| | 之前 | 之后 |
|---|---|---|
| js_fetch.c | 529 行（三个概念混在一起） | 326 行（fetch 逻辑 + 生命周期） |
| js_headers.c | 不存在 | 181 行（独立模块） |
| js_response.c | 不存在 | 142 行（独立模块） |
| js_loop.c | 224 行（调度 + HTTP + Promise） | 85 行（纯调度） |

总行数从 753 到 734——差不多。**但复杂度的分布完全不同了。** 每个文件只做一件事，每个模块只管自己的复杂度。

## 同一个手法，第二次生效

回头对比第五篇和这一篇：

| | 第五篇：封装事件引擎 | 这一篇：封装 fetch |
|---|---|---|
| 散落的复杂度 | epoll_wait 在两个文件各写一遍 | fetch 行为散落在 loop 里 |
| 封装到哪 | `js_epoll_poll()` | `js_fetch_complete/fail/on_read/on_write` |
| 被简化的 | 调用者从 20 行变 1 行 | loop 从 224 行变 85 行 |

同一个手法，同样的效果。**被封装的模块消化了自己的复杂度，周围的模块卸掉了不属于自己的负担。** 三步——建立边界、归还行为、切断依赖——每一步都让系统更清晰一点。

## 封装复杂性——一个可复用的架构工具

如果这篇文章只带走一样东西，那就是这个工具。

**什么时候该用：**

- 一个模块在做不属于它的事——调度器在做 HTTP 解析，Controller 在做数据库查询
- 一个文件里有多个独立变化的概念——Headers 和 fetch 没有理由绑在一起
- 相同的逻辑在不同路径里重复——complete 和 fail 各写一遍清理

**怎么用：**

三个动作，有顺序。**建立边界**——让每个概念有自己的作用域，内部细节对外不可见。**归还行为**——把逻辑搬回它所属的模块，让创建者管理生命周期。**切断依赖**——用回调或接口替代具体类型引用，让模块之间只通过抽象通信。先建边界，再搬行为，最后切依赖——顺序不能反。边界不清楚的时候搬行为，只是把混乱从一个地方搬到另一个地方。

**怎么检验：**

- 这个模块能不能不知道那个模块的存在？（loop 不知道 fetch）
- 如果加一个新的同类事物，现有代码需要改吗？（加 WebSocket，loop 不用动）
- 每个文件是不是只因为一个理由而改变？

这些问题不限于 C，不限于系统编程。**任何语言、任何项目，当你觉得代码"改不动"或者"一改就牵一发动全身"，大概率是复杂度散落在了错误的地方。** 找到它，搬回去。手法就这一个，但你会用很多次。

---

GitHub: https://github.com/hongzhidao/jsbench
