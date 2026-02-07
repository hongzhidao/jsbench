# 一个 nginx 工程师接手 AI 写的压测工具

AI 写了一个类似 wrk 的可编程压测工具，测试全过，能跑。但仔细一看——请求全部失败却报告零错误，同步阻塞伪装成异步，架构经不起推敲。一个 nginx 工程师逐步重构它的记录。

## 文章

<!-- 每篇文章提供中英文两个版本 -->

1. [AI 写了一个类似 wrk 的可编程压测工具](01-ai-wrote-my-benchmark-tool.zh.md)
2. [重构 AI 生成的 C 代码：一个 nginx 工程师的标准](02-architecture-is-everything-in-ai-era.zh.md)
3. [AI 写的代码全部通过测试，但每个请求都失败了](03-improving-architecture-by-solving-problems.zh.md)
4. [好的架构就是刚刚好：不多不少](04-what-is-good-architecture.zh.md)

---

# An nginx Engineer Took Over AI's Benchmark Tool

AI built a programmable alternative to wrk. Tests passed. It ran. But look closer — every request failed yet reported zero errors, synchronous blocking masqueraded as async, and the architecture couldn't hold up. This series documents an nginx engineer refactoring it step by step.

## Articles

<!-- Each article is available in both Chinese and English -->

1. [AI Built a Programmable Alternative to wrk](01-ai-wrote-my-benchmark-tool.en.md)
2. [Refactoring AI-Generated C: An nginx Engineer's Standards](02-architecture-is-everything-in-ai-era.en.md)
3. [AI Code Passed Every Test, but Every Request Failed](03-improving-architecture-by-solving-problems.en.md)
4. [Good Architecture Is Just Enough — No More, No Less](04-what-is-good-architecture.en.md)
