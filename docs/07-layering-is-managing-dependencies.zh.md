# 连接层里住着一个 HTTP 解析器

前两篇打磨了引擎层——`js_epoll_poll()` 封装了事件分发，`js_engine_t` 组装了 epoll 和定时器。引擎层现在很干净。

但往上看一层，我发现了一个不对劲的东西。

## 连接结构体里有什么

看 `js_conn_t`——这是 jsbench 连接层的核心结构：

```c
typedef struct js_conn {
    js_event_t           socket;
    conn_state_t         state;
    SSL                 *ssl;

    const char          *req_data;
    size_t               req_len;
    size_t               req_sent;

    js_http_response_t   response;   /* ← 这是什么？ */

    uint64_t             start_ns;
    int                  req_index;
    void                *udata;
} js_conn_t;
```

socket、状态、TLS、读写缓冲——这些都是连接该有的。但 `js_http_response_t response` 是什么？一个 HTTP 响应解析器，直接嵌在连接结构体里。

连接结构体里住着一个 HTTP 解析器。这意味着：**这个"连接"，不能离开 HTTP 独立存在。**

再看 `conn_do_read()`——连接层处理读事件的函数：

```c
static int conn_do_read(js_conn_t *c) {
    char buf[JS_READ_BUF_SIZE];

    for (;;) {
        ssize_t n;
        if (c->ssl) {
            n = js_tls_read(c->ssl, buf, sizeof(buf));
        } else {
            n = read(c->socket.fd, buf, sizeof(buf));
        }

        /* ... 错误处理 ... */

        int ret = js_http_response_feed(&c->response, buf, (size_t)n);
        if (ret == 1) {
            c->state = CONN_DONE;    /* HTTP 解析完成 → 连接状态变更 */
            return 1;
        }
    }
}
```

读数据是连接层的事，解析 HTTP 是协议层的事。但在这个函数里，两件事混在一起，没有边界。

还有 `js_conn_keepalive()`：

```c
bool js_conn_keepalive(const js_conn_t *c) {
    const char *conn_hdr = js_http_response_header(&c->response, "Connection");
    if (conn_hdr && strcasecmp(conn_hdr, "close") == 0)
        return false;
    return true;
}
```

连接该不该复用，这个函数去读了 HTTP `Connection` 头。连接层依赖了协议层的知识。

三个地方，同一个问题：**conn 和 http 塌成了一层。**

## 这意味着什么

jsbench 从下往上有三层：

```
  ┌─────────────────┐
  │      http        │  协议语义：解析响应、判断 keep-alive
  └────────┬─────────┘
           │ 依赖
  ┌────────▼─────────┐
  │      conn        │  传输管理：连接、读写、TLS
  └────────┬─────────┘
           │ 依赖
  ┌────────▼─────────┐
  │     engine       │  事件驱动：epoll + timer
  └──────────────────┘
```

理想状态下，每层做自己的事，依赖方向从上往下。http 用 conn 发数据，conn 用 engine 做 I/O。上层依赖下层，下层不知道上层的存在。

但现在的代码是这样的：

```
  ┌──────────────────────────────┐
  │       conn + http             │
  │                               │
  │  conn 内嵌 http response      │
  │  conn 读 HTTP 头判断 keep-alive│
  │  conn 直接调 http 解析器       │
  └──────────────┬────────────────┘
                 │
  ┌──────────────▼────────────────┐
  │           engine               │  ← 这层是干净的
  └───────────────────────────────┘
```

上面两层塌成了一坨。改 HTTP 解析逻辑要动 conn 的代码，改 conn 的读取策略可能碰坏 HTTP 解析。**两层的复杂度不是加在一起，是乘在一起。**

对比一下 engine 层。engine 层经过第五、六篇的重构，依赖方向是干净的：`js_event_t` 不知道上面是谁，通过回调隔离。第六篇引入 `js_engine_t` 的时候，改 engine 的内部结构（从 thread-local epfd 到显式 engine 参数），上层的连接逻辑完全不受影响。**改下层不影响上层——这就是分层的回报。**

conn 层呢？如果你想换一种 HTTP 解析方式，或者想让 jsbench 支持非 HTTP 的协议，你会发现改不动——因为 conn 和 http 的代码纠缠在一起，改哪边都怕碰坏另一边。

**分层解决的核心问题是依赖。不是消除依赖，是让依赖单向。** 依赖一旦单向，每层就能独立理解、独立修改、独立演进。

## 为什么 AI 会写成这样

这个问题值得想一想。AI 写代码的目标是"让功能工作"。让一个 HTTP 客户端工作，最直接的方式就是：创建连接、发请求、读数据、解析响应、判断 keep-alive。这些步骤串起来，放在一组函数里，功能确实工作了。

但"让功能工作"和"让系统可演进"是两个不同的目标。分层是为后者服务的。**层间的边界不是功能需求，是架构决策。** AI 不会主动说"这里该切一刀，把传输和协议分开"——因为不切也能工作。

这也是前几篇的经验。第五篇 engine 层的重构，不是因为 AI 改不了 epoll 的代码，而是 AI 不会主动说"epfd 不该是参数，应该是线程的属性"。人给了方向，AI 执行得很好。conn 和 http 的分层也一样：AI 完全有能力做拆分，但"该不该拆、在哪里拆"，是人的判断。

## 去掉不合理

方向明确了：把 conn 和 http 分开。从哪里下手？

改进架构有一个很朴素的方法：**找到不合理的地方，去掉它。** `conn_do_read()` 不应该做 HTTP 解析——去掉。`js_conn_t` 不应该内嵌 HTTP 响应——去掉。`js_conn_keepalive()` 不应该读 HTTP 头——去掉。一个一个去掉，层就自然分开了。

背后的道理很简单：**一个函数做的事越多，它牵扯的概念就越多，改动它的理由也越多。** 这有个经典的名字叫单一职责，但我更愿意用朴素的说法：让每个函数只做一件它该做的事。

当然这不是死规矩。`conn_do_write()` 写完数据之后设置 `c->state = CONN_READING`——写完切换到读，这是连接状态机的自然流转，没必要硬拆。**判断的标准不是"做了几件事"，而是"这些事会不会各自独立变化"。** 读字节和解析 HTTP 显然会——你完全可能换一种协议解析，或者换一种读取策略，它们各自有各自的变化理由。

## 第一步：引入 buffer，让读和解析分开

`conn_do_read()` 要改——让它只做读数据，不做解析。但读到的字节放哪里？

现在用的是栈上临时数组，读完立刻喂给解析器，用完就丢。如果不立刻处理，字节就需要一个地方存着。引入 `js_buf_t`：

```c
typedef struct {
    char   *data;
    size_t  len;     /* 已有数据的长度 */
    size_t  cap;     /* 分配的容量 */
} js_buf_t;
```

有了 buffer，`conn_do_read()` 变成了：

```c
static int conn_do_read(js_conn_t *c) {
    js_buf_t *in = &c->in;

    for (;;) {
        js_buf_ensure(in, in->len + JS_READ_BUF_SIZE);

        ssize_t n;
        if (c->ssl) {
            n = js_tls_read(c->ssl, in->data + in->len, in->cap - in->len);
        } else {
            n = read(c->socket.fd, in->data + in->len, in->cap - in->len);
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            c->state = CONN_ERROR;
            return -1;
        }
        if (n == 0) return 1;  /* 对端关闭 */

        in->len += (size_t)n;
    }
}
```

没有 `js_http_response_feed`，没有 `c->state = CONN_DONE`，没有任何协议相关的判断。**只做一件事：把字节从 socket 读到 buffer 里。**

HTTP 解析搬到了调用方——worker 和 loop 读完之后，从 `c->in` 取数据，自己喂给 HTTP 解析器。不再是 conn 推数据给解析器，而是调用者从 conn 的 buffer 里拉数据。**这是整个分层工作中最关键的一步——conn 不再知道 HTTP 的存在。**

代码变更: [47e4d2c](https://github.com/hongzhidao/jsbench/commit/47e4d2c)

## 第二步：给 conn 层一个正式的家

之前连接相关的代码都在 `js_http_client.c` 里——文件名本身就暴露了问题：一个叫"HTTP 客户端"的文件，怎么能是一个干净的传输层？

新建 `js_conn.h` 和 `js_conn.c`，提取 `js_conn_read()` 作为第一个公开接口。`js_http_client.c` 里还留着 create、free、write、reuse、reset 这些函数，没有急着一次搬完。

我一直在有意识地避免过度设计。设计不足的代码是诚实的——它告诉你"这里还没想清楚"。过度设计的代码是伪装的——那些抽象可能猜错了未来的方向，等你真的需要改的时候，反而比没有抽象更难动。**在没有足够信息的时候，少做比多做安全。**

代码变更: [5c8c3bc](https://github.com/hongzhidao/jsbench/commit/5c8c3bc)

## 第三步：去掉结构体里不该有的字段

行为解耦了，模块也有了。但 `js_conn_t` 里还嵌着 `js_http_response_t response`。行为上分开了，结构上还绑着。

好的结构体应该是简洁的——**它拥有的每个字段都应该是它需要的，而不是别人需要的。**

去掉之后，HTTP 响应放哪里？答案在已有的机制里。`js_event_t` 有一个 `void *data` 字段，让调用者自己持有 response，通过 `socket.data` 关联到连接：

```c
js_http_response_init(&responses[i]);
conns[i]->socket.data = &responses[i];
```

读事件处理里，从 `socket.data` 取出 response：

```c
static void worker_on_read(js_event_t *ev) {
    js_conn_t *c = (js_conn_t *)ev;
    js_http_response_t *r = c->socket.data;
    /* ... */
}
```

`js_conn_keepalive()` 也搬到 worker 里，变成局部函数 `worker_keepalive()`。连接层不再需要知道 HTTP 协议的任何细节。

去掉 `response` 之后的 `js_conn_t`：

```c
typedef struct js_conn {
    js_event_t       socket;
    conn_state_t     state;
    SSL             *ssl;

    const char      *req_data;
    size_t           req_len;
    size_t           req_sent;

    js_buf_t         in;

    uint64_t         start_ns;
    int              req_index;
    void            *udata;
} js_conn_t;
```

没有任何 HTTP 相关的字段。连接就是连接。**结构体的简洁程度，就是分层是否到位的直接体现。**

代码变更: [0bbe6a3](https://github.com/hongzhidao/jsbench/commit/0bbe6a3)

## 第四步：把类型搬回它该在的地方

`js_conn_t` 不再依赖 `js_http_response_t`，终于可以从 `js_main.h` 搬到 `js_conn.h`。之前搬不了，就是因为 `js_http_response_t` 定义在 `js_main.h` 中，内嵌了就搬不走。依赖去掉之后，障碍消失了。

conn 层开始有自己的轮廓了：类型在 `js_conn.h`，读实现在 `js_conn.c`，不依赖任何协议层的东西。create、free、write 这些函数还留在 `js_http_client.c` 里，但方向已经很清楚了。不急，一步一步来。

代码变更: [edc2859](https://github.com/hongzhidao/jsbench/commit/edc2859)

## 回头看

四步做完，回到开头的问题：连接层里还住着那个 HTTP 解析器吗？

不在了。`js_conn_t` 里没有任何 HTTP 字段，`conn_do_read()` 不调任何 HTTP 函数，conn 模块有了自己的头文件和实现文件。conn 不知道 http 的存在——就像 engine 不知道 conn 的存在一样。

每一步做的都是同一件事：**找到不合理的依赖，去掉它。** 不需要提前设计完美的分层方案，不需要一次搬完所有代码。看到一个不合理的地方，去掉它，系统就比之前好一点。积累几步，层就自然分开了。

分层不是一种理论，是一种实用的手段。判断分层是否到位，有几个直观的检验方法：

- **看结构体。** 每个字段都是自己需要的，还是替别人拿着的？
- **看函数。** 它做的事情是否属于同一层？读字节和解析协议不在同一层。
- **看依赖方向。** 下层有没有引用上层的类型或调用上层的函数？
- **看头文件。** 一个模块的 `.h` 能不能不依赖不相关的类型就编译通过？

这些检验不需要画架构图，打开代码就能看到。

---

GitHub: https://github.com/hongzhidao/jsbench
