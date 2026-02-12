# 用结构体表达理解

代码能跑，测试能过，功能正常——但总觉得哪里不对劲。这种感觉在之前几篇里反复出现过：conn 里嵌着 HTTP 解析器，两个请求类型做同一件事，借用指针的生命周期说不清。每次深入去看，根源都一样——**某个概念应该存在于代码中，但没有。**

这一篇讲的就是怎么找到那些缺失的概念。

## 谁在用 conn

**第一条：C 路径的 worker。** worker 创建连接，设回调，在回调里读数据、喂 HTTP 解析器、记统计、判断 keep-alive、管连接复用。这条路径很直接。

**第二条：JS 路径的 fetch。** fetch 解析 URL、DNS 解析、创建连接，然后调 `js_loop_add()` 把连接交给 loop。从这一刻起，fetch 就不管了。读写、解析、完成判断、超时处理——全在 loop 里。

上一篇已经说过：loop 不该管 HTTP，fetch 该管。但在改之前，先看看 C 路径。它虽然更简单，但同样有问题。

## C 路径：start_ns 不该在 conn 里

worker 在请求完成后算延迟：

```c
uint64_t elapsed_ns = js_now_ns() - c->start_ns;
```

`start_ns` 存在 `js_conn_t` 里。但请求计时是 HTTP 层的事，不是传输层的事。如果 conn 做了 keep-alive 重连、TLS 重握手，那些时间算不算？连接层不该知道。

更根本的问题：worker 的回调从 `c->socket.data` 拿 HTTP 响应，从 `c` 拿计时——**一次请求的状态分散在两个地方。**

该有一个结构体来表达"一次 HTTP 交互的对端状态"：

```c
typedef struct {
    js_http_response_t  response;
    uint64_t            start_ns;
} js_http_peer_t;
```

response 和 start_ns 放在一起，因为它们属于同一个概念：一次请求/响应的交互。`start_ns` 从 `js_conn_t` 里删掉——conn 干净了一点。

同时，HTTP 相关的类型定义——`js_header_t`、`js_http_response_t`、`js_http_peer_t`——从 `js_main.h` 搬到了 `js_http.h`。HTTP 终于有了自己的头文件。

代码变更: [180ecbd](https://github.com/hongzhidao/jsbench/commit/180ecbd)

## JS 路径：fetch 的概念不存在于代码中

看 `js_loop_add()` 的签名：

```c
int js_loop_add(js_loop_t *loop, js_conn_t *conn,
                SSL_CTX *ssl_ctx, JSContext *ctx,
                JSValue resolve, JSValue reject);
```

六个参数，散着传进来。loop 在函数内部分配一个结构体，把这些参数填进去，再初始化 HTTP 响应解析器、设置超时、注册 epoll。

一个事件循环，干着 HTTP 客户端的初始化。为什么？因为**代码里没有一个结构体表达"一次 fetch 操作是什么"。**

理解了 fetch 是什么之后，三个概念自然浮现：

**fetch 操作**——一次完整的 HTTP 请求/响应。它拥有连接、响应解析器、超时定时器。

**pending 接口**——loop 调度一个待完成操作所需的最少信息。promise 回调、链表节点、loop 引用。

**线程上下文**——engine 是每个线程一个的基础设施。不该属于 loop，也不该属于 worker，是线程本身的属性。

三个概念，三个结构体：

```c
typedef struct {
    js_engine_t  *engine;
} js_thread_t;

typedef struct {
    SSL_CTX             *ssl_ctx;
    JSContext           *ctx;
    JSValue              resolve;
    JSValue              reject;
    js_loop_t           *loop;
    struct list_head     link;
} js_pending_t;

typedef struct {
    js_http_response_t   response;
    js_pending_t         pending;
    js_conn_t           *conn;
    js_timer_t           timer;
} js_fetch_t;
```

每个字段都有归属。conn 和 timer 是 fetch 的，不是 loop 的。engine 是线程的，不是 loop 的。pending 是 loop 调度的接口，只包含 loop 需要的东西。

有了这些结构体，`js_loop_add()` 从六个参数变两个。分配和初始化自然发生在 `js_fetch()` 里。超时处理从 loop 搬回 fetch。`struct js_loop` 从四个字段缩减到一个——一个 `struct list_head`。

这个改动很大，细节放到下一篇展开。

代码变更: [3ec7f15](https://github.com/hongzhidao/jsbench/commit/3ec7f15)

## 结构体是对领域的理解

回头看这两个改动——引入 `js_http_peer_t` 和引入 `js_fetch_t`——做的是同一件事：**看到一个领域概念没有被代码表达出来，给它一个结构体。**

DDD（领域驱动设计）说：代码应该反映你对问题域的理解。这个道理在 C 语言里格外直接——你对系统的理解，最终体现为 `struct` 的定义。

`start_ns` 和 `response` 散在两个地方，是因为没有"HTTP 对端"这个概念。有了 `js_http_peer_t`，它们自然归位。conn、timer、response 混在 loop 的结构体里，是因为没有"fetch 操作"这个概念。有了 `js_fetch_t`，它们自然归位。engine 挂在 loop 和 worker 上各存一份，是因为没有"线程上下文"这个概念。有了 `js_thread_t`，它自然归位。

**结构体不是存储容器，是对领域概念的表达。** 字段放在哪个结构体里，体现的是你对"谁拥有什么"的理解。理解对了，字段就在对的地方；理解错了或者理解缺失，字段就会散落在不该在的地方——就像前面看到的那些问题。

这也解释了为什么 AI 不太会主动做这种改进。AI 写代码的目标是"让功能工作"，它会把数据放在最方便的地方——哪里要用就放哪里。但"方便"和"正确"不是一回事。把 `start_ns` 放在 conn 里很方便，但不正确。把 fetch 的分配放在 loop 里很方便，但不正确。**识别领域概念、判断归属、做出结构决策——这是人的工作。**

## 难在哪

说起来一句话——找到正确的模型。做起来很难。

难在"正确"不是显而易见的。`js_http_peer_t` 看起来简单——response 加 start_ns，谁都觉得合理。但在它出现之前，没人觉得 `start_ns` 放在 conn 里有什么问题。代码能跑，测试能过，功能正常。**问题不是"错了"，是"没有意识到可以更好"。**

更难的是识别出那些真正核心的模型。`js_fetch_t` 不只是把几个字段换个位置——它重新定义了 fetch 和 loop 的边界，改变了分配策略、超时处理的归属、函数签名。一个结构体的引入，触发了一连串的变化。**好的模型能简化复杂性，差的模型会制造复杂性。** 区别在于你是否真正理解了问题域里的概念边界。

这种能力没有捷径，但有方法。

**多实践。** 不是写更多的代码，是写完之后回头看——这个结构体真的对吗？这些字段真的属于这里吗？本系列从第一篇到现在，每篇都是在重新审视已有的代码。重构不是写完之后的额外工作，是理解深化之后的自然结果。

**看好的作品。** nginx 的 `ngx_connection_t` 不知道 HTTP 的存在；Linux 内核的 `struct sock` 不知道 TCP 的存在。好的代码库里，结构体的定义本身就是一份领域模型的文档。看它们怎么划分概念边界，比看任何设计模式的书都直接。

**多思考。** 写代码的时候停下来问自己：这个东西是什么？它拥有什么？它属于谁？这些问题听起来很简单，但认真回答它们，往往就能发现结构上的问题。本篇的每个改动，都是从这样一个简单的问题开始的。

DDD 的书很厚，概念很多。但落到 C 语言里，核心就一句话：**想清楚问题域里有哪些概念，每个概念该拥有什么，然后用 struct 表达出来。**

## 带走一个方法

如果这篇文章只能带走一样东西，那就是这个问题：**这个字段为什么在这个结构体里？**

下次写代码或者读代码的时候，盯着一个结构体看，逐个字段问自己：它描述的是这个结构体所代表的概念吗？还是它其实属于别的东西，只是恰好放在了这里？

如果一个结构体里有不属于它的字段，要么是缺少一个概念——需要引入新的结构体；要么是概念的边界画错了——需要重新划分。

回到之前的检验方法——看结构体、看函数、看依赖方向。这一篇加一条：**看字段的归属。** 每个字段都应该属于它所在的概念，不多不少。做到了，模块边界、函数职责、代码归属，很多问题都会自然解决。

数据结构理清了，但 loop 里还有 `loop_on_read`——读数据、喂 HTTP 解析器、判断完成状态——这些行为仍然属于 fetch。下一篇会进入 JS 的深水区：让 `js_fetch_t` 真正接管它该有的职责。

---

GitHub: https://github.com/hongzhidao/jsbench
