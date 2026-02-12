# 一个 nginx 工程师的 AI 实战：从有代码到有架构

AI 写了一个类似 wrk 的可编程压测工具，测试全过，能跑。但仔细一看——请求全部失败却报告零错误，同步阻塞伪装成异步，架构经不起推敲。一个 nginx 工程师逐步重构它的记录。

## 完整系列

如果你对某个具体话题感兴趣：

- **AI 写的代码长什么样？** → [AI 写了一个类似 wrk 的可编程压测工具](01-ai-wrote-my-benchmark-tool.zh.md)
- **怎么让 AI 改进架构？** → [AI 写的代码全部通过测试，但每个请求都失败了](03-improving-architecture-by-solving-problems.zh.md)
- **什么是好的架构？** → [好的架构就是刚刚好：不多不少](04-what-is-good-architecture.zh.md)
- **事件引擎怎么封装？** → [AI 盖楼很快，但图纸得人来画](05-event-engine-refactoring.zh.md)
- **怎么选数据结构？** → [功能没变，但引擎造好了](06-building-the-work-engine.zh.md)
- **分层到底在分什么？** → [分层到底在分什么](07-layering-is-managing-dependencies.zh.md)
- **结构体设计的方法** → [代码能跑，但缺了三个概念](09-expressing-understanding-through-structs.zh.md)
- **怎么封装复杂性？** → [封装 fetch：让复杂度回到它该在的地方](10-refactoring-fetch.zh.md)

完整系列十一篇：「一个 nginx 工程师接手了 AI 的压测工具」

1. [AI 写了一个类似 wrk 的可编程压测工具](01-ai-wrote-my-benchmark-tool.zh.md)
2. [重构 AI 生成的 C 代码：一个 nginx 工程师的标准](02-architecture-is-everything-in-ai-era.zh.md)
3. [AI 写的代码全部通过测试，但每个请求都失败了](03-improving-architecture-by-solving-problems.zh.md)
4. [好的架构就是刚刚好：不多不少](04-what-is-good-architecture.zh.md)
5. [AI 盖楼很快，但图纸得人来画](05-event-engine-refactoring.zh.md)
6. [功能没变，但引擎造好了](06-building-the-work-engine.zh.md)
7. [分层到底在分什么](07-layering-is-managing-dependencies.zh.md)
8. [改进 HTTP 设计](08-improving-http-design.zh.md)
9. [代码能跑，但缺了三个概念](09-expressing-understanding-through-structs.zh.md)
10. [封装 fetch：让复杂度回到它该在的地方](10-refactoring-fetch.zh.md)
11. [回头看](11-looking-back.zh.md)

---

# An nginx Engineer's AI Practice: From Having Code to Having Architecture

AI built a programmable alternative to wrk. Tests passed. It ran. But look closer — every request failed yet reported zero errors, synchronous blocking masqueraded as async, and the architecture couldn't hold up. This series documents an nginx engineer refactoring it step by step.

## Full Series

1. [AI Built a Programmable Alternative to wrk](01-ai-wrote-my-benchmark-tool.en.md)
2. [Refactoring AI-Generated C: An nginx Engineer's Standards](02-architecture-is-everything-in-ai-era.en.md)
3. [AI Code Passed Every Test, but Every Request Failed](03-improving-architecture-by-solving-problems.en.md)
4. [Good Architecture Is Just Enough — No More, No Less](04-what-is-good-architecture.en.md)
5. [AI Builds Fast, but Humans Draw the Blueprints](05-event-engine-refactoring.en.md)
6. [Nothing Changed, but the Engine Is Built](06-building-the-work-engine.en.md)
7. [Layering Is Managing Dependencies](07-layering-is-managing-dependencies.en.md)
8. [Improving HTTP Design](08-improving-http-design.en.md)
9. [The Code Works, but Three Concepts Are Missing](09-expressing-understanding-through-structs.en.md)
10. [Refactoring Fetch: Putting Complexity Where It Belongs](10-refactoring-fetch.en.md)
11. [Looking Back](11-looking-back.en.md)
