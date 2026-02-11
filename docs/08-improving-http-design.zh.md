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

## 分还是合

上一篇把连接和 HTTP 分开了，这一篇把请求和 URL 合到一起了。方向相反，判断标准是同一个：**看它们会不会各自独立变化。**

连接的读写和 HTTP 的解析各有各的变化理由——换协议不用动连接层。分。

请求和它的 URL 永远一起变——改了 URL 要重新序列化，改了 method 也要。没有场景需要单独改一个不改另一个。合。

什么该分，什么该合，不是看流程有几步，而是看变化的方向是不是一致的。**一起变的东西放一起，各自变的东西分开。** 分层、模块化、内聚——都是这一句话。

## 回头看这一步

改动本身很小——合并两个类型，消除重复，理清所有权。但它背后有几个值得记住的东西。

**怎么发现问题。** 不需要什么高级分析。把模块列出来，看看谁有自己的头文件，谁没有——HTTP 没有。再看类型，两个结构体描述同一件事，信息在它们之间重复——这就是信号。**散落和重复，是设计问题最直观的表现。**

**分和合是同一个判断。** 上一篇分（连接和 HTTP），这一篇合（请求和 URL）。听起来矛盾，其实标准一样：各自独立变化的就分开，一起变化的就放一起。不是"分比合高级"，也不是"越细越好"。

**好的基础设施会被自然复用。** `js_buf_t` 是为 conn 层的读缓冲引入的，这里用来存序列化结果，刚好合适。没有刻意设计"通用 buffer"——它就是一个简单的字节容器，但因为足够简单，所以到处都能用。**基础设施的价值不在于设计得多精巧，而在于它足够简单，以至于不需要解释就能复用。**

请求这边暂时理顺了。但回头看开头列的那张清单——HTTP 散落在七八个文件里的问题，才刚开始解决。

代码变更: [ae6d011](https://github.com/hongzhidao/jsbench/commit/ae6d011)

---

GitHub: https://github.com/hongzhidao/jsbench
