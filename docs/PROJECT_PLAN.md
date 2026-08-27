# Maelstrom — C++17 高频撮合引擎与限价订单簿

> 为 2027 量化开发实习准备的核心项目。覆盖国内量化私募 JD 最高频出现的能力组合：
> **现代 C++ 工程 + 限价订单簿 + 连续撮合 + 无锁并发 + 低延迟优化 + 性能压测**。
>
> 时间窗口：约 8 周（每周 15–20h，总 120–160h）。

---

## 一、为什么是这个项目（JD 匹配 + 背景匹配）

### JD 能力对照

| JD 高频关键词 | 项目对应模块 | 你的现状缺口 |
|---|---|---|
| 现代 C++（11/14/17）、模板、RAII | 全项目，工程脚手架 | 有但缺系统级 C++ 项目 |
| 限价订单簿、price-time priority | W1–W2 LOB | 无 |
| 撮合引擎、连续撮合、订单类型 | W3–W4 Matching | 无 |
| 无锁、atomic、memory order | W5 SPSC 队列 | 并发概念有，C++ 无锁是空白 |
| cache 友好、false sharing、低延迟 | W6 优化 | 性能优化有，C++ 低延迟场景没用过 |
| benchmark、P99、吞吐 | W7 压测 | 有方法论经验 |
| CMake、测试、CI、架构文档 | W1/W8 工程化 | 华为实习已有基础 |

### 背景匹配

- **算法强**（ICPC/CCPC 银牌）→ 订单簿数据结构（hash + 链表 + 有序 price level）正好发挥，O(1) 增删查是你能做漂亮的点。
- **系统性能优化有经验**（华为背压/降级/门控、Rust 有界通道）→ 无锁/低延迟优化思路相通，迁移成本低。
- **缺系统级 C++ 工程** → 这是"多文件 + CMake + 测试 + 并发"的标准系统项目，正好补齐。
- **缺量化领域** → 撮合/订单簿是量化最核心的领域知识，一个项目同时拿到"领域知识 + C++ 系统"两块拼图。

> 网络层 / FIX / ITCH / 回测不放进主 2 个月窗口——它们是重要的，但不是"低延迟交易核心"的最短路径。做完主项目后，每个都可以作为后续 1–2 周的增量扩展模块，继续丰富简历。

---

## 二、总体架构

```
┌──────────────────────────────────────────────────────────┐
│                   Maelstrom 撮合核心                      │
│                                                          │
│   ┌──────────────┐   SPSC 无锁队列   ┌────────────────┐  │
│   │  Ingest 线程 │ ────────────────→│  撮合线程(单线程)│  │
│   │ (订单/行情输入)│   ring buffer   │  MatchingEngine │  │
│   └──────┬───────┘                  └────────┬───────┘  │
│          │                                   │          │
│     Order/MarketData 输入            Match/Trade/L2 事件 │
│          │                                   │          │
│          │                          ┌─────────┴─────────┐│
│          │                          │   EventBus        ││
│          │                          │ (回测/日志/统计订阅)││
└──────────┼──────────────────────────┴───────────────────┘│
```

### 核心组件

1. **LimitOrderBook**：订单簿数据结构，O(1) 增删查，price-time FIFO。
2. **MatchingEngine**：撮合逻辑，限价/市价/IOC/FOK/POST-ONLY，price-time priority，自成交防护。
3. **LockFreeSpscQueue**：无锁环形队列，隔离输入线程与撮合线程。
4. **EventBus**：撮合事件（成交/撤单/L2 更新）分发。
5. **Benchmark/Harness**：压测、延迟分布 P50/P95/P99、吞吐 orders/sec。

---

## 三、8 周计划（每模块含：目标 / C++ 学习点 / JD 关键词 / 产出）

### W1（15–20h）工程脚手架 + 订单簿骨架

- **目标**：C++17 + CMake + GoogleTest + clang-tidy + CI 全套环境；强类型 Order/Price/Quantity；订单簿接口骨架与 price level 双向链表。
- **C++ 学习点**：现代 CMake target-based 写法、GoogleTest、clang-tidy 规则；C++17 move semantics、`std::optional`/`std::variant`/`std::string_view`、`constexpr`、强类型（`enum class` / PhantomType）。
- **JD 关键词**：现代 C++17、工程规范、强类型、构建系统。
- **产出**：可编译运行的空骨架、CI 绿灯、强类型基础。

### W2（15–20h）限价订单簿完整实现

- **目标**：`unordered_map<OrderId, Order*>` + 同价位双向链表 + price level 有序容器，实现 add/cancel/modify，全部 O(1)（同价位 FIFO，跨价位有序）。
- **C++ 学习点**：intrusive list、`unordered_map` 性能与自定义 hash/allocator、避免悬垂指针（iterator 稳定性）、自定义 arena/pool allocator 入门。
- **JD 关键词**：限价订单簿、price-time priority、O(1) 数据结构。
- **产出**：完整订单簿 + 单元测试覆盖（插入/撤销/修改/边界）。

### W3（15–20h）撮合引擎核心逻辑

- **目标**：限价单连续撮合（cross best opposite side）；市价单、IOC、FOK、POST-ONLY；生成 Match/Trade 事件与 best bid/ask 更新。
- **C++ 学习点**：RAII 管理资源、事件驱动/观察者模式、错误处理（异常 vs `std::expected`/error code）、`[[nodiscard]]`/契约式设计。
- **JD 关键词**：撮合引擎、连续撮合、订单类型、市场微观结构。
- **产出**：撮合核心 + 事件输出 + 单元测试。

### W4（15–20h）撮合完善 + 事件总线 + 集成测试

- **目标**：事件总线（多订阅者）；自成交防护、空单、超量等边界；多组订单序列集成测试验证撮合正确性。
- **C++ 学习点**：测试驱动、property-based/fuzz testing 基础、并发安全的接口设计（为 W5 铺路）。
- **JD 关键词**：撮合正确性、自成交防护、事件驱动架构。
- **产出**：可端到端跑通的撮合 + 事件总线 + 集成测试。

### W5（15–20h）无锁 SPSC 队列 + 并发架构

- **目标**：无锁环形队列（head/tail 用 `std::atomic`，acquire-release 序）；Ingest 线程 → 撮合线程隔离；正确性论证（为什么无锁正确）。
- **C++ 学习点**：《C++ Concurrency in Action》2e 无锁章节、memory order（relaxed/acquire/release/acq_rel/seq_cst）、ABA 问题（SPSC 无此问题，理解为何）、`alignas` 入门。
- **JD 关键词**：无锁、atomic、memory order、SPSC、生产者-消费者。
- **产出**：并发撮合核心 + 无锁正确性测试。

### W6（15–20h）低延迟优化

- **目标**：cache line 对齐消除 false sharing（`alignas(64)`）、热路径 SoA 数据布局、内联与分支提示、自定义 arena allocator 减少 malloc。
- **C++ 学习点**：mechanical sympathy（cache/分支预测/prefetch）、Data-Oriented Design、Agner Fog 优化手册、`[[gnu::always_inline]]`/`__builtin_expect`、allocator 设计。
- **JD 关键词**：cache 友好、false sharing、数据布局、低延迟、SIMD（可选）。
- **产出**：优化版撮合核心 + 优化前后对比数据。

### W7（15–20h）性能压测 + 调优

- **目标**：Google Benchmark micro-bench；延迟分布 P50/P95/P99、吞吐 orders/sec；perf 火焰图定位瓶颈；对比 `std::map` 基线实现量化提升；可选 SIMD(AVX2) 批量成交。
- **C++ 学习点**：性能剖析方法论、benchmark 陷阱（warmup、CPU 频率、测量偏差）、perf/flamegraph。
- **JD 关键词**：benchmark、P99、吞吐、性能调优、SIMD。
- **产出**：完整 benchmark 报告 + 性能数字。

### W8（15–20h）工程化 + 简历打磨 + 可选回测

- **目标**：CMake 完整、CI 门禁、覆盖率 >80%；README 架构图 + benchmark 报告；可选 tick replay 简化版（喂历史订单序列回放）；简历 bullet 提炼。
- **C++ 学习点**：CI/CD、覆盖率工具、技术写作。
- **JD 关键词**：CI、覆盖率、架构文档、可维护性。
- **产出**：可投简历的项目仓库 + 简历 bullet。

---

## 四、性能目标（参考值，实测后填入简历）

| 指标 | 目标 | 备注 |
|---|---|---|
| 单次撮合延迟 P50 | < 50 ns | 热路径 |
| 撮合延迟 P99 | < 200 ns | 尾部延迟 |
| 吞吐 | > 10 M orders/sec | 单线程撮合核心 |
| 订单操作复杂度 | O(1) | 插入/撤销/查找 |
| 较 `std::map` 基线延迟降低 | ≥ 5× | 量化数据结构收益 |

> 数字是参考，不是承诺。目标是让实测数字落在"能讲出技术故事"的量级（纳秒级撮合、千万级吞吐），与简历一贯的量化成果风格匹配。

---

## 五、简历包装（草稿，最终用 W7 实测数字替换占位符）

### Maelstrom — C++17 高频撮合引擎与限价订单簿
github.com/ArrogHie/Maelstrom

独立设计并实现 C++17 高频交易核心引擎，覆盖限价订单簿、连续撮合、无锁并发、低延迟优化：

- **限价订单簿数据结构**：基于 `unordered_map` + 双向链表 + 有序 price level 实现 O(1) 订单插入/撤销/查找，支持 price-time priority FIFO；自定义 arena allocator 降低热路径 heap 分配开销
- **撮合引擎**：支持限价/市价/IOC/FOK/POST-ONLY 与连续撮合，事件驱动输出成交事件与 L2 best 更新，含自成交防护与边界处理
- **无锁并发**：SPSC 无锁环形队列隔离行情接入与单线程撮合核心，基于 `std::atomic` + acquire-release memory order 保证无锁正确性，热路径无等待
- **低延迟优化**：cache line 对齐（`alignas(64)`）消除 false sharing、热路径 SoA 数据布局、内联与分支提示，单次撮合 P50 **<X ns**、吞吐 **>Y M orders/s**，较 `std::map` 基线延迟降低 **Z×**
- **工程规范**：CMake + GoogleTest 单元/集成测试覆盖率 **>80%**、clang-tidy 零警告、CI 门禁、Google Benchmark 性能报告

---

## 六、学习资源清单

### 现代 C++ / 并发 / 无锁（必读）
- 《C++ Concurrency in Action》2e — Anthony Williams（无锁 / memory order 圣经，W5 主力）
- 《Effective Modern C++》— Scott Meyers（C++11/14，W1 配套）
- cppreference.com（随手查）
- CppCon talks：搜 "lock-free"、"data-oriented design"
- mechanicalsympathy.com — Martin Thompson（cache / false sharing，W6 必看）

### 撮合 / 订单簿 / 市场微观结构
- GitHub 搜 `orderbook` / `limit order book` / `matching engine`（看实现思路，别抄）
- NASDAQ TotalView ITCH 协议文档（行业事实标准，免费 PDF）
- FIX 4.4 协议规范（FIX Protocol Ltd，免费）
- 《Trading and Exchanges: Market Microstructure for Practitioners》— R. Harris（经典理论）

### 性能剖析
- Google Benchmark（官方文档）
- `perf` / flamegraph（Linux；Windows 可用 WSL2）
- Agner Fog 优化手册（免费，CPU 微架构 / 指令延迟）

---

## 七、后续扩展模块（主项目做完后，每个 1–2 周增量）

按对国内量化私募 JD 的加分度排序：

1. **FIX 协议解析器**（C++23 `std::expected` / zero-copy 解析）— 直接命中 JD"FIX 协议"。
2. **ITCH 行情解码器**（NASDAQ 二进制协议，SIMD 加速解析）— 命中"行情/市场数据"。
3. **tick replay 回测框架**（event-driven，喂历史订单序列回放策略）— 命中"回测"。
4. **multicast 行情网关**（epoll + UDP multicast，零拷贝）— 命中"网络/Linux/行情分发"。

每个模块独立可演示、可写简历，做完主项目后按兴趣和投递节奏挑选。

---

## 八、关键决策记录（供后续参考）

- **C++ 版本**：C++17（主流、稳定、JD 出现频率最高；部分高阶特性如 `std::expected` 留给扩展模块上 C++23）。
- **不做**：策略 alpha（那是研究员的活，开发岗不要求）、实盘交易（无意义且有风险）、复杂网络层（时间不够，留扩展）。
- **重点**：可量化的性能数字 + 工程规范 + 能讲清的技术故事（为什么这么设计、memory order 怎么选、false sharing 怎么消）。
