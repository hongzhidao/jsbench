# 改进 HTTP 的设计

前几篇从底往上清理了三层。engine 有 `js_engine.h`，conn 有 `js_conn.h`，buffer 有 `js_buf.h`，timer 有 `js_timer.h`——每个模块都有自己的头文件，边界清晰，依赖单向。

但 HTTP 呢？

## HTTP 散落在各处

看一下 HTTP 相关的代码都住在哪里：

- **请求的类型定义**——在 `js_main.h` 里，和几十个其它类型挤在一起
- **请求的序列化**——在 `js_util.c` 里，和 URL 解析、时间格式化等工具函数混在一起
- **响应的类型定义**——也在 `js_main.h` 里
- **响应的解析**——在 `js_http_parser.c` 里
- **连接的创建和管理**——在 `js_http_client.c` 里（但上一篇已经把 HTTP 从 conn 剥离了，这个文件名本身就不对了）
- **keep-alive 判断**——在 `js_worker.c` 里
- **响应数据的喂入**——在 `js_worker.c` 和 `js_loop.c` 里各写了一遍

engine 层有自己的家，conn 层有自己的家。**HTTP 没有家。** 它的代码散落在七八个文件里，没有统一的头文件，没有清晰的模块边界。

对比一下：

```
engine 层:  js_engine.h + js_engine.c + js_epoll.h + js_epoll.c   ← 干净
conn 层:    js_conn.h + js_conn.c                                  ← 干净
HTTP 层:    散落在 js_main.h, js_util.c, js_http_parser.c,         ← 散
            js_http_client.c, js_worker.c, js_loop.c, js_fetch.c
```

这不是一次能清理完的。这一篇先从请求开始。

## 请求：两个类型做一件事

jsbench 发请求分两步：先从 JS 脚本提取请求信息（URL、method、headers、body），再序列化成 HTTP 报文字节。

AI 为这两步定义了两个结构体：

```
js_request_desc_t   → 请求的意图：url(字符串), method, headers, body
js_raw_request_t    → 序列化结果：data(报文字节), len, url(解析后的)
```

两步流程，两个类型。能用，但有问题。

**URL 存了两次。** desc 里有 URL 字符串，raw 里有解析后的 URL 结构体。序列化的时候两者都要传入——同一个请求，拆成两个参数。

**序列化结果承担了不该有的职责。** `js_raw_request_t` 本该只是报文字节，但它还存了一份解析后的 URL，供后续 DNS 解析和连接创建使用。一个叫"raw request"的东西，承担了传递地址信息的职责。

**所有权不清晰。** 请求描述的字段有些是堆分配的，有些是字面量。清理的时候靠注释提醒自己哪些该释放：

```c
/* method: only free if we strdup'd it */
/* This is a simplification -- in production we'd track ownership */
```

当你需要注释来解释内存该不该释放，说明所有权模型有问题。

三个问题，同一个根源：**同一件事被拆成了两个类型。**

## 合并

方向很简单：两个类型合成一个。

```
js_request_t   → url(解析后的), method, headers, body
```

关键变化：**url 从字符串变成了解析后的结构体，直接作为请求的一部分。** 不再需要单独传 URL，不再需要在序列化结果里存一份。

序列化函数从四个参数变三个——请求信息全在一个对象里，结果写到 `js_buf_t`（之前引入的 buffer 抽象自然复用了）。清理用一个 `js_request_free()`，所有字段统一分配、统一释放，不需要注释解释。

`js_raw_request_t` 消失了。config 里存的从"报文字节 + URL"变成了纯粹的 `js_buf_t` 字节数组。URL 提升到 config 层面——所有请求共享同一个目标地址，存一份就够。Worker 不再需要 `config->requests[0].url.host` 这种间接路径，直接用 `config->url.host`。

七个文件，净减 21 行。行为不变。

代码变更: [ae6d011](https://github.com/hongzhidao/jsbench/commit/ae6d011)

## 连接的输出：从借用到拥有

请求统一之后，再看连接层。`js_conn_t` 发送数据靠三个字段：

```c
const char *req_data;   /* 指向外部数据 */
size_t      req_len;
size_t      req_sent;
```

连接不拥有这块数据，只借了一个指针。Worker 路径上没问题——数据在 config 里，生命周期比连接长。但 fetch 路径上就乱了：序列化的报文是堆分配的，连接借了指针，`js_loop.c` 还得单独存一份 `raw_data` 来负责释放。

同一块内存，两个地方操心它的生命周期。

解法很直接：**让连接拥有自己的输出 buffer。**

```c
js_buf_t out;   /* 连接拥有的输出缓冲 */
js_buf_t in;    /* 连接拥有的输入缓冲 */
```

`js_buf_t` 加一个 `pos` 字段记录已发送位置，替代原来的三个字段。`js_conn_set_request()` 改名 `js_conn_set_output()`——连接不需要知道自己发的是 HTTP 还是别的什么。这和上一篇把响应从 conn 剥离是同一个方向：**conn 层不该知道 HTTP。**

fetch 路径变干净了：序列化完拷进 conn 的 buffer，原始数据立即释放。`js_loop.c` 里三处 `free(p->raw_data)` 全部消失。

代码变更: [7cb01d6](https://github.com/hongzhidao/jsbench/commit/7cb01d6)

## 连接层的归位

上一篇末尾留了一句：create、free、write 还在 `js_http_client.c` 里，方向很清楚，不急。

现在可以动了。输出 buffer 替代 `req_data` 之后，`js_http_client.c` 里的函数全是纯连接操作——create、free、set_output、process_write、reuse、reset。没有一个跟 HTTP 沾边。一个叫"HTTP 客户端"的文件，里面没有 HTTP。

全部搬进 `js_conn.c`。合并时发现两个文件各有一份 `conn_try_handshake()`——读路径和写路径各抄了一份。统一成一份，由调用方决定 handshake 之后做什么。

`js_http_client.c` 删除。回头看开头那张表：

```
conn 层:  js_conn.h + js_conn.c   ← 完整了
```

上一篇给 conn 一个家，这一篇把全家搬进去了。

代码变更: [8492906](https://github.com/hongzhidao/jsbench/commit/8492906)

## 回头看

三步改动，两条线索。

第一条：**理清所有权。** 两个请求类型合一个，消除数据重复和释放歧义。给 conn 加输出 buffer，消除借用指针的生命周期问题。C 语言里没有借用检查器，所有权全靠人理——模型越简单，出错越少。

第二条：**让代码回家。** 请求有了统一的类型 `js_request_t`。连接的全部操作回到了 `js_conn.c`。一个模块的代码散落在多个文件里，就像散落的工具——不是不能用，但每次都要找。

**分和合是同一个判断。** 上一篇分（conn 和 HTTP），这一篇合（请求和 URL、conn 的实现文件）。标准一样：**各自独立变化的分开，一起变化的放一起。** 连接读写和 HTTP 解析各有各的变化理由——分。请求和 URL 永远一起变——合。create、free、write 全是连接操作——合到一个文件。

**好的基础设施会被自然复用。** `js_buf_t` 是为 conn 的读缓冲引入的。这一篇里它先存序列化结果，又做 conn 的输出缓冲，加一个 `pos` 字段就够了。**基础设施的价值不在于设计得多精巧，而在于足够简单，以至于不需要解释就能复用。**

请求和连接都理顺了。但回头看开头的清单——HTTP 散落的问题，还有不少要解决。

## 下一步在哪

我原本想在这一篇里把 HTTP 的问题全部解决。但做完这三个改动之后发现，又碰到了之前类似的情况——有些地方改不动，硬改只会让代码更乱。

这和之前封装 epoll 时一样：想直接推进目标，却发现前面挡着一堆前置的耦合问题。所以这三个改动——请求统一、输出 buffer、连接归位——其实是绕路。不是不想直奔主题，而是路还没修好。

不过，切入点很清楚。

## 谁用了 conn

conn 是传输层。要改 HTTP 的设计，得先搞清楚谁在用 conn、怎么用的。

翻了源码，两个地方在用。

**第一条：worker C 路径** (`js_worker.c`)

worker 创建连接，设置读写回调。在回调里读数据、喂给 HTTP 解析器、判断完成或错误。请求完成后，记统计，判断 keepalive，决定 reuse 还是 reconnect。

这条路径很直接。**worker 发请求、收响应、管连接。** 职责清晰，没有多余的层。

**第二条：JS 路径的 fetch** (`js_fetch.c` + `js_loop.c`)

fetch 解析 URL、DNS 解析、创建连接、序列化请求。然后调 `js_loop_add()`，把连接交给 loop。

从这一刻起，fetch 就不管了。

loop 设置读写回调。在回调里读数据、喂 HTTP 解析器、判断完成或错误。完成后 resolve promise，失败则 reject。

停一下。

fetch 创建了连接，但不管连接。读写、解析、完成判断、错误处理——**全在 loop 里。** loop 是事件循环，一个本该通用的基础设施。但它现在知道 `js_http_response_feed`，知道 `HTTP_PARSE_BODY_IDENTITY`，知道 peer close 是完成还是错误。

对比一下：engine 也是事件处理的基础设施，它完全不知道 conn，更不知道 HTTP。**这才是干净的。**

loop 不该管 HTTP。**该管 HTTP 的是 fetch。**

## 这篇只能到这里

方向看到了，但改动不小。`loop_on_read` 和 `worker_on_read` 几乎完全相同——读数据、喂解析器、状态转换——要让 fetch 接管 HTTP 职责，得先把这份重复逻辑抽出来，再重新划分边界。这不是改几行代码的事，放到下一篇。

回头看这篇的三个改动——请求合并、输出 buffer、连接归位——都不是从架构图推导出来的，而是一行行读源码读出来的。看到两个结构体做一件事才想到合并，看到借用指针的生命周期问题才加 buffer，看到文件名和内容不符才做了归位。现在发现 loop 不该管 HTTP，也是读着代码觉得不对劲，深入之后才明白的。

**读源码的能力没有捷径。** 多读好代码，多读自己的代码，慢慢培养出对"不对劲"的敏感。架构有时候就是一种审美——说不清为什么觉得不舒服，但直觉告诉你有问题。然后去找原因，改掉了，就舒服了。

---

GitHub: https://github.com/hongzhidao/jsbench
