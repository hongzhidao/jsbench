# AI 写了一个类似 wrk 的可编程压测工具

压测工具这个东西，做后端的都用过。但你有没有发现一个尴尬的现状？

wrk，性能顶级，但脚本用 Lua。想做个"先登录拿 token 再压接口"的场景？写到你怀疑人生。

k6，JS 脚本，功能齐全。但装完几十 MB，还得学一套自定义 API——`http.get()`、`check()`、`sleep()`。我只想压一个接口，不想学一套框架。

我想要的很简单：wrk 的性能，JS 的灵活，零学习成本。

所以我自己写了一个：**jsbench**。

## 三行代码压一个接口

```js
export default 'http://localhost:3000/api';

export const bench = { connections: 100, duration: '10s', threads: 4 };
```

```bash
jsb bench.js
```

没了。

需要 POST？改 export default：

```js
export default {
    url: 'http://localhost:3000/api/users',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: 'test' })
};
```

需要先登录拿 token 再压接口？用 async function：

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

注意：没有任何新 API。`fetch`、`export default`、`async/await`——都是你已经会的。

## 底层：C + QuickJS

jsbench 用 C 写，嵌入了 Fabrice Bellard 的 QuickJS 引擎。

JS 脚本在 QuickJS 里执行，解析出请求配置后交给 C 层的 epoll 事件循环去发。网络层是纯 C 的非阻塞 I/O，支持 HTTPS/TLS，多线程各跑各的 epoll 实例。

编译产物是一个静态链接的二进制文件，没有运行时依赖，拿来就用。

## 性能：跟 wrk 一个级别

先说大家最关心的。jsbench 的网络层是纯 C 写的——epoll 事件循环、非阻塞 I/O、多线程各跑各的实例，跟 wrk 的架构一样。JS 只负责生成请求配置，真正发压力的是 C。所以在静态压测（固定 URL/固定 body）场景下，jsbench 的 QPS 跟 wrk 在同一量级。

这不是"接近"，是同一个级别。因为瓶颈在网络和内核，不在工具本身。

## 跟 wrk / k6 的对比

|            | wrk        | k6              | jsbench          |
|------------|------------|-----------------|------------------|
| 语言       | C          | Go              | C                |
| 脚本       | Lua（有限）| JS（自定义 API）| JS（标准 fetch） |
| 性能       | 顶级       | 中等            | 顶级（同架构）   |
| 二进制大小 | 小         | 大（~60MB）     | 小               |
| 复杂场景   | 困难       | 支持            | 支持             |
| 学习成本   | 低         | 中              | 零               |

jsbench 不是要替代 k6——k6 有云服务、有 dashboard、有完整的生态。jsbench 的定位是：wrk 级别的性能，JS 的灵活性，你只想快速压一下，写个 JS 就搞定。

## 关于 AI 编程，聊点真话

这个项目从零到可用，一天，基本是 AI 写的。工具是 Claude Code + Opus。

但我不想写成"AI 太强了"的爽文。先说一句：**AI 编程这件事，很多人比我用得好。** 网上到处能看到惊艳的案例——几小时做出完整产品、生成精美 UI、搭建复杂系统。跟他们比，我的用法很朴素。我只是一个在 nginx 团队做后端的，拿 AI 做了个自己领域的小工具。所以下面说的只是个人体验，不代表 AI 编程的上限。

### 我的做法

我做的事不多。先把需求想清楚，写成 README，定义好 API 和脚本格式。然后准备了架构文档给 AI 做上下文——不过这些文档本身也是 AI 生成的。唯一算"干预"的，就是告诉它文件命名风格，这是我在 nginx 团队养成的习惯。除此之外，全程让 AI 自己写。

有一点值得说：**让 AI 写测试用例，是目前保证 AI 代码质量的最好方式。** 你不用逐行审代码，跑一遍测试就知道写得对不对。

### AI 写代码，到底行不行？

先说好的。命名——编程界公认的难题。但对连自然语言都能搞定的 AI 来说，给变量、函数起个准确的名字，反而是它最轻松的部分。这块交给它，基本没问题。

逻辑也还行。但好的代码追求的是简洁和清晰，这一点 AI 还有距离。

我有个感觉：**AI 的审美和人类是不一样的。** 人类追求简洁，某种程度上是因为大脑资源有限——我们不得不发展出一套最适合人类理解的方式。而 AI 有近乎暴力式的算力，它不需要这种妥协。就像象棋里的 AI，它的下法已经不是人类的思路了，但它赢了。

我相信 AI 会形成自己的编程风格——严谨、有效，但不一定符合人类的审美。

### AI 最弱的地方：架构

代码怎么组织、模块怎么拆、抽象放在哪一层——这是 AI 目前最明显的短板。我从 AI 编程刚能用的时候就开始用了，每个版本都在进步，但到现在，这块依然不太让我满意。

现在有一种玩法：两个 AI 同时干活，一个写代码，一个做 review，互相对抗来提高质量。这个项目我没这样做，因为我更想看 AI 独立完成的代码到底长什么样。

接下来我会继续维护这个项目，自己动手改架构。**人写的代码和 AI 写的代码，到底会有什么不同？** 有兴趣的可以关注。

### AI 编程是趋势，但效果会有滞后期

有一点我比较确定：AI 编程不是炒作，是真的趋势。但技术趋势有个规律——**它带来的真正效果，往往有滞后期。** 就像互联网刚出来的时候，大家觉得它能改变一切，然后泡沫破了，很多人说不过如此。结果十年后，它真的改变了一切。

AI 编程现在可能也在这个阶段。工具每个月都在进步，但要真正改变软件开发的方式，可能还需要一段时间来沉淀。最终的形态会是什么样？程序员的角色会变成什么？说实话我也不知道，但我很好奇。

## 开源

GitHub: https://github.com/hongzhidao/jsbench

欢迎 Star，欢迎提 Issue。

对了，jsbench 不只是压测工具，它也是自己的测试工具——整个项目的测试用例就是用 jsbench 自身跑的。用自己测自己，这大概是最诚实的 dogfooding 了。

还有，这篇文章基本也是 AI 生成的。我只提供了观点，组织语言的活也让它干了。所以你看，连吐槽 AI 这件事，都是 AI 自己写的。
