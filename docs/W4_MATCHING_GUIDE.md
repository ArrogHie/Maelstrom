# W4 手写任务单 —— 事件总线、自成交防护与集成验收

> 这是一份**练习册**，不是参考手册。W3 你交付了一个会成交的市场：
> 五种订单类型、price-time 撮合、`SubmitResult` 回执。但它有两个明显的"盲区"：
> **成交的事只有一个人知道**（`set_trade_handler` 是单订阅，风控/行情/日志想同时听就没了办法），
> 以及**它会自己吃自己**（同一账户的两笔单在簿上照样成交——真实交易所把这当成事故）。
> W4 要给它装上"麦克风与风控"：**EventBus** 把每笔成交/挂单/撤单/拒绝广播给任意多订阅者，
> **自成交防护（STP）** 让引擎学会在执行接触点上拦下自己的单，最后用**随机差分**把三者一起锁死。
>
> 每个任务仍按固定四段组织：
>
> **① 问题** → 你此刻面对的具体困难或现象（先想清楚"为什么要有这一步"）
> **② 提示** → 怎么动手的线索（不是完整答案，卡住再往下读）
> **③ 理想效果** → 完成后应该能看到什么（命令输出 / 测试颜色）
> **④ 答案** → 放在文末附录，编号对应。写不出或写完对照，**别复制粘贴，手打一遍**
>
> 规则重申：所有实现代码由你手写进 `include/` 和 `src/`。`tests/` 里已备好**裁判测试**
> （AI 隔离编写，只做验收、不进你的实现目录）：W4 共 76 个用例，分四块——
> 语义单测 25 个 + 生命周期压力 3 个（`MatchingW4.*`）、事件总线 8 个（`EventBusW4.*`）、
> 40 组随机差分对拍 `Seeds/MatchingDifferentialW4.*`（你的引擎 vs 带账户和 STP 的慢速参考引擎，
> 逐步比对 SubmitResult / **完整事件流** / 簿状态）。
> 语义契约见下方"判据"，你的代码和它必须逐字一致。
> **回归铁律：W1–W3 的 151 个测试必须保持全绿**——你正在给已上线的数据结构加字段、
> 给已有的撮合循环加逻辑，旧测试就是你的安全网。

---

## 0. 先认识问题：W4 到底在解决什么

W3 交付后，引擎的核心循环是对的，但有三个真问题：

| # | 问题 | 后果 |
|---|---|---|
| 1 | **结果只有一个人知道**：`set_trade_handler` 是单槽位，换 handler 旧的就没 | 风控、行情分发、日志、回测统计都想订阅成交——单回调装不下，"事件驱动架构"无从谈起 |
| 2 | **引擎会自己吃自己**：同一账户挂卖 100 买 100 照样成交 | 真实市场里这是操纵价格/误操作的源头，交易所都有 Self-Match Prevention（STP）；JD 里"自成交防护"是高频词 |
| 3 | **功能叠加后正确性悬空**：账户、策略、事件都是新状态，组合爆炸 | 手写用例永远枚举不完"FOK 撞上自己的挂单""IOC 连撤两笔自己的单"这类交叉场景——只有随机差分能兜底 |

W4 的四步解法：

```
①  给 Order 加 account 字段           → "谁的单"有了落脚点（0 = 未设置，永不触发 STP）
②  定义 Event / EventBus             → 事件有形状，总线有多订阅/退订/顺序保证
③  引擎加 STP + 事件发布点            → 执行接触点拦截 + submit/cancel 全程广播
④  裁判测试（隔离）                  → 语义 + 总线 + 生命周期/UB + 随机差分，四层验收
```

一句面试话术：**W3 让引擎"做对的事"，W4 让引擎"把自己做的事如实、有序地告诉所有关心的人，
并且不给自己开后门"。**

---

## 1. 判据：测试裁判的规则（先通读，别跳）

下面每一条都被 `tests/test_matching_engine_w4.cpp` 和 `tests/naive_matching_engine_w4.h` 强制。
任何一条没满足，测试就红；红的时候回来对照这里。

### 1.1 类型与字段

- `types.h` 新增强类型 `AccountId`（与 `OrderId` 同款：`AccountId{int64}`、`get()`、`==`）。
- `Order` 新增 `AccountId account{0};`，**必须加在 `type` 之后且带默认值**——
  W1/W2 的四段、W3 的五段聚合初始化必须原样编译通过。**`account == 0` 表示"未设置账户"，
  永不触发自成交防护**（这也是 W3 差分测试不需要改一个字就能继续跑的原因）。
- `SelfMatchPolicy : std::uint8_t { CancelNewest, CancelOldest }`，引擎默认 `CancelNewest`。
- `EventKind : std::uint8_t { Trade, OrderRested, OrderCanceled, OrderRejected }`。
- `using SubscriptionId = std::uint64_t;`，订阅号**从 1 开始、单调递增、不复用**。
- `Event` 结构（字段顺序固定，测试逐字段比对）：

| 字段 | 类型 | Trade | OrderRested | OrderCanceled | OrderRejected |
|---|---|---|---|---|---|
| `kind` | `EventKind` | Trade | OrderRested | OrderCanceled | OrderRejected |
| `order_id` | `OrderId` | 主动单 id | 该单 id | 被撤/取消的单 id | 该单 id |
| `account` | `AccountId` | 主动方账户 | 该单账户 | 被撤单账户 | 该单账户 |
| `side` | `Side` | 主动方向 | 该单方向 | 被撤单方向 | 该单方向 |
| `counterparty_id` | `OrderId` | 被动单 id | `{0}` | `{0}` | `{0}` |
| `price` | `Price` | 成交价（被动价） | 挂单价 | `{0}` | `{0}` |
| `quantity` | `Quantity` | 成交量 | 挂簿量 | 被撤/取消的量 | `{0}` |
| `reason` | `RejectReason` | None | None | None | 拒绝原因 |

  与 `kind` 无关的字段保持默认值（`{0}` / `None`）。`Event` 是带默认成员初始化的聚合，
  **没有默认值的字段就是 bug**——W3 差分测试抓过一次"未初始化的 reason"（教训还热着）。

### 1.2 自成交防护（STP）

- **触发条件**：撮合循环中，主动单即将吃掉对手队头时（**执行接触点**，只看队头这一笔），
  `aggressor.account != 0 && passive.account == aggressor.account` 即触发。
  账户不同、或任一方为 0 → 不触发，正常成交。**负数账户是非 0**，同样触发。
- **只在接触点判定，不预扫全簿**：自己 deeper 价位的挂单在没被"即将吃到"之前不拦截。
- **`CancelNewest`**：主动单**余量全部取消**（此前已成交的部分保留）；
  该队头挂单**纹丝不动**；余量不挂簿。outcome = `Cancelled`（resting=0）；
  发一个 `OrderCanceled{主动单 id, quantity=取消的余量}`。
- **`CancelOldest`**：把队头挂单**整单撤销**（等价一次撤单：摘链/删空档/释放，
  发 `OrderCanceled{被撤单 id, quantity=其全部剩余量}`），撮合**继续**——
  同价下一笔如果又是自己的，再次触发，依次后撤。
- **策略只影响其后的 submit**：`set_self_match_policy` 随时可切。
- **预检不看账户（刻意取舍）**：POST-ONLY 预检遇到**自己账户**的最优价照样拒
  （`PostOnlyWouldCross`）；FOK 预检的 `matchable_quantity` 把自己挂单也计入可用量。
  于是可能出现"FOK 通过预检、执行时被 STP 砍掉余量"→ outcome = `Cancelled` 且 filled < quantity。
  这是定义好的行为，差分测试覆盖。为什么这么设计？预检回答的是"市场有没有这么多流动性"，
  STP 回答的是"这些流动性里哪些不能碰"——把它们耦合会让预检变成 O(own orders) 的扫描，
  而真实交易所也普遍是执行时点判定。

### 1.3 事件发布点

- 每次 `submit` 的事件序列：
  1. `n` 个 `Trade` 事件（n = 成交笔数，按撮合顺序）；
  2. **恰好一个终态事件**：
     - 余量挂簿（Limit/PostOnly，含部分成交后）→ `OrderRested{quantity=挂簿量, price=挂单价}`；
     - 余量被取消（Market/IOC 余量、STP-CancelNewest 砍掉的余量）→ `OrderCanceled{quantity=余量}`；
     - **全额成交 → 不发终态事件**（事件流 = 市场可观察事实：簿变了、单被拒了才有事可报；
       Filled 除了成交没有任何其他事实）。
- 被拒的 `submit` → **恰好一个** `OrderRejected{reason=...}`，零其他事件。
- `cancel(id)` 成功 → 恰好一个 `OrderCanceled{quantity=该单全部剩余量}`；失败（不在簿）→ 零事件。
- STP-CancelOldest 每撤一笔自己的挂单发一个 `OrderCanceled`（可能连续多个）。
- **W3 的 `set_trade_handler` 通道与事件总线相互独立**：两者都会收到成交，
  互不影响（handler 只收 Trade，不收终态事件）。
- **分发时机与顺序**：同步；事件在 `submit`/`cancel` 返回前全部发完；
  多订阅者按**订阅注册顺序**逐一回调。
- 引擎未连接任何订阅者时照常撮合（发布到空总线是 no-op）。

### 1.4 EventBus API

```cpp
class EventBus {
public:
    using Handler = std::function<void(const Event &)>;
    SubscriptionId subscribe(Handler handler);   // 从 1 递增，不复用
    bool unsubscribe(SubscriptionId id);         // 不存在返回 false（含二次退订）
    void publish(const Event &event);            // 按注册顺序同步分发
    std::size_t subscriber_count() const;
};
```

- 引擎持有 `EventBus` 成员，对外转发：`subscribe(handler)` / `unsubscribe(id)` /
  `set_self_match_policy(policy)`。
- **分发期间不允许 subscribe/unsubscribe**（重入改订阅集合是未定义行为，测试不制造这个场景；
  W5 上并发时这里要升级成正式契约）。

### 1.5 校验（不变，重申）

W3 的校验顺序原样保留：`quantity <= 0 → InvalidQuantity`；
`orderId 已在簿 → DuplicateOrderId`；非市价 `price <= 0 → InvalidPrice`；
POST-ONLY 交叉 → `PostOnlyWouldCross`；FOK 不可全额 → `FokNotFillable`。
**`account` 不参与校验**（任何 int64 都是合法账户）。每个拒绝路径发一个 `OrderRejected`。

### 1.6 其他

- 余量挂簿**带上主动单的账户**（`Order r = o` 自然携带）；
  此后别人撞上这本余量时按其账户判 STP。
- 被撤/被砍的订单 id 照常可复用。
- `MatchingEngine` 依旧不可拷贝、可移动。
- 事件类型全部放进 `matching_engine.h`（不新建头文件——`Event` 需要 `RejectReason`，
  拆头会引入循环包含；真想拆，先把 `RejectReason` 抽到独立头，作为思考题）。

---

## 2. 任务概览（做完一个打一个勾）

| # | 任务 | 改哪个文件 | 预计 |
|---|---|---|---|
| 1 | `AccountId` + `Order.account`（保持旧初始化兼容） | `include/Maelstrom/types.h` `order.h` | 30 min |
| 2 | `SelfMatchPolicy` / `Event` / `EventBus` 类型设计 | `include/Maelstrom/matching_engine.h` | 1–1.5 h |
| 3 | STP 进撮合循环（执行接触点判定） | `src/matching_engine.cpp` | 2–3 h |
| 4 | 引擎事件发布点（submit / cancel 全程） | `src/matching_engine.cpp` | 1–2 h |
| 5 | 把裁判挂进构建 | `tests/CMakeLists.txt` | 10 min |
| 6 | 语义 + 总线全绿 + 口试自检 | 只读不写 | 1–2 h |
| 7 | 内存/UB 门禁 + 差分全绿 + 提交 | 只读不写 | 1–2 h |

---

## 任务 1 ｜ 给 Order 装上账户

**① 问题**

STP 的前提是知道"这单是谁的"。和 W3 加 `type` 时一模一样的困境：`Order` 是聚合类型，
W1/W2/W3 的代码都在用四段/五段初始化，插字段或加构造函数都会炸一片。

**② 提示**

1. `types.h` 里照抄 `OrderId` 的模板加 `AccountId`（构造、`get()`、`==`/`!=`）。
   强类型防的就是"把账户 id 当订单 id 传进去"这种事故——两个 int64 别名在 ABI 上没差，
   在类型系统里是两个世界。
2. `order.h` 里**最后一行**加 `AccountId account{0};`——放最后、带默认值、不加构造函数，
   三条纪律沿用 W3 任务 1。0 的语义在判据 1.1：未设置，永不触发 STP。
3. 想清楚为什么 0 要豁免：如果默认账户也参与 STP，所有没设置账户的老订单互相都"自成交"，
   W3 的差分测试当场全红。0 = 匿名单，这是**向后兼容**和**领域语义**的交点。

**③ 理想效果**

```bash
cmake --build build -j
ctest --test-dir build    # W1-W3 的 151 个测试仍全绿
```

**④ 答案**：附录 A1。

---

## 任务 2 ｜ 事件类型与总线（类型设计先行）

**① 问题**

事件系统最容易写成"随手往回调里塞参数"。你需要先回答：事件有哪几种？
每种哪些字段有意义？订阅怎么标识、怎么退订、按什么顺序收？
这些问题不先定清楚，写引擎发布逻辑时会反复返工。

**② 提示**

1. `EventKind` 四个值别多别少（判据 1.1）。注意"全额成交不发终态事件"——
   事件流的口径是**市场可观察事实**，不是"给每个 outcome 配一个事件"。
2. `Event` 用 tag + 字段的结构体而不是 `std::variant`：字段固定、可以整体写日志/memcmp，
   简单可调试；variant 是后续扩展方向（W8 之后的事）。
   每个字段给默认成员初始化（`{0}` / `None`）——W3 差分测试抓过未初始化的 `reason`，
   默认值是类型契约的一部分。
3. `EventBus` 就是一个 `vector<槽位>` + 自增订阅号：subscribe 尾插、unsubscribe 按 id 删、
   publish 按序遍历。它是**头文件内联实现**的小类，不需要 cpp。
  分发期间改订阅集合未定义（判据 1.4），文档里写清楚即可，不用加锁。
4. 为什么 `Event` 要放 `matching_engine.h`？它需要 `RejectReason`（W3 定义的），
   拆新头文件会循环包含。内聚在一个头里，CMake 一行都不用改。

**③ 理想效果**

```bash
cmake --build build -j        # 库能编过
```

你现在应该能口头背出：`Event` 八个字段分别是什么、四种事件各自哪些字段有值、
订阅号从几开始、分发按什么顺序。

**④ 答案**：附录 A2。

---

## 任务 3 ｜ 自成交防护进撮合循环

**① 问题**

W3 的撮合循环是一条直线：取队头 → 判交叉 → 吃。STP 要在这条直线的"吃"之前
插一个判定点，而且两种策略的行为完全不同（一个杀主动单、一个杀被动单）。
难点不是写，是**边界**：撞上自己时余量还挂不挂？连续撞上两笔自己的单呢？
FOK 已经通过预检了还要不要执行？判定提前到"扫全簿"行不行？

**② 提示**

1. 判定点只有一个位置：循环内、`consume_best` 之前、交叉判定之后。
   判据 1.2 的触发条件照抄：`account != 0 && best->account == o.account`。
2. `CancelNewest`：发 `OrderCanceled{主动单, quantity=remaining}` → 置一个
   `self_killed` 标记 → break。**余量处理必须排除被砍的单**：can_rest 要看这个标记，
   否则被砍的余量又挂回簿上（差分会抓到）。outcome 走 W3 的老规则自然落 `Cancelled`
   （resting=0、filled<original），不用新分支。
3. `CancelOldest`：发 `OrderCanceled{队头单, quantity=其剩余量}` → `book_.cancel(队头id)`
   → `continue`（不消耗 remaining）。注意终止性：每轮要么撤一笔（簿缩小）、要么吃一笔、
   要么 break，不会死循环。
4. 预检（任务 3 里）**一个字都不改**：POST-ONLY 和 FOK 继续对账户失明。
   判据 1.2 的"刻意取舍"段讲清楚了为什么。
5. "只在接触点判定"意味着：自己 deeper 价位的单不拦截——先吃别人的，走到自己那笔才触发。

**③ 理想效果**

```bash
cmake --build build -j
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.Stp*'   # 先绿这一族
```

做完后你能回答：**"CancelNewest 之后我的余量去哪了？CancelOldest 连续撞三笔自己的单
发几个事件？"**（前者： nowhere，被砍了，标记保证不挂簿；后者：三个 `OrderCanceled`。）

**④ 答案**：附录 A3。

---

## 任务 4 ｜ 引擎事件发布点

**① 问题**

STP 只改了循环中部；事件是**全程**的：拒绝要报、挂簿要报、Market/IOC 余量被取消要报、
撤单要报——发布点散落在 submit/cancel 的每个出口。漏一个，差分测试的"事件数不一致"
就会红；多发一个（比如 STP 砍单后余量又发一次取消），一样红。

**② 提示**

1. 数一数发布点：**被拒（5 种原因，一个 lambda 统一发）、每笔成交、余量挂簿、
   余量被取消（Market/IOC）、STP 两种取消、cancel 成功**。共 7 处。
   沙盒验证时 AI 自己的第一版就漏了"Market/IOC 余量取消"这一处——差分红了一片，
   这个发布点最容易漏。
2. 拒绝路径用 lambda 收口：`auto reject = [&](RejectReason r) { 发 OrderRejected; return res; };`
   五个 return 全走它，事件字段（order_id/account/side/reason）不会发岔。
3. `cancel()` 要先 `book_.find(id)` **捕获**账户/方向/量，再撤、再发事件——
   顺序反了就是拿着已释放的数据发事件（ASan 会教你做人）。
4. 全额成交不发终态事件；别顺手"补对称"。
5. W3 的 `handler_(t)` 原样保留，与 `bus_.publish(Trade事件)` 并列——两条独立通道。

**③ 理想效果**

```bash
cmake --build build -j
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.EventSequence*:MatchingW4.CancelEmits*:MatchingW4.BusAndTradeHandler*'
```

**④ 答案**：附录 A3（与任务 3 同一段参考实现）。

---

## 任务 5 ｜ 把裁判挂进构建

**① 提示**

1. `tests/CMakeLists.txt` 的 `add_executable` 源列表加一行 `test_matching_engine_w4.cpp`
   （和 W3 时一样，只改这一行）。`naive_matching_engine_w4.h` 是头文件，被测试源 include，不用登记。
2. 库目标**不用改**：W4 没有新的 .cpp，也没有新的头文件（判据 1.6）。
3. 重新配置再构建：

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DMAELSTROM_BUILD_TESTS=ON -DMAELSTROM_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

4. 认一下过滤尺子：

```bash
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.*'             # 语义 + 生命周期（28）
./build/tests/maelstrom_tests --gtest_filter='EventBusW4.*'             # 总线（8）
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.Stp*'          # STP 一族
./build/tests/maelstrom_tests --gtest_filter='Seeds/MatchingDifferentialW4.RandomSubmitSequence/7'
ctest --test-dir build --output-on-failure                              # 全量（含 W1-W3，共 227）
```

**③ 理想效果**

构建目前红着——**预期内的红**，是起跑线。你能说出 W4 交付的 =
`Order.account`、`AccountId`、`Event`/`EventBus`、`MatchingEngine` 的
STP（两策略）+ 事件发布 + 订阅转发。

**④ 答案**：附录 A4（一处改动，一行）。

---

## 任务 6 ｜ 语义 + 总线全绿 + 口试自检

**① 提示**

1. 跑语义与总线：

```bash
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.*:EventBusW4.*'   # 28 + 8 = 36
```

2. 失败时按族缩小（`Stp*` / `EventSequence*` / `Huge*` / `Lifetime*`）。
   差分失败信息带 seed / step / 事件序号。
3. **口试自检**（写进笔记）：
   - 为什么 `account == 0` 永不触发 STP？如果默认账户也参与，W3 的哪些测试会红、为什么？
   - CancelNewest 和 CancelOldest 在"成交序列、事件序列、outcome"三个维度上各有什么差别？
   - 为什么 STP 判定放在执行接触点而不是下单前扫全簿？两种做法的复杂度和语义差别？
   - FOK 通过了预检却被 STP 砍成 `Cancelled`——这是 bug 还是契约？为什么预检对账户失明？
   - 为什么全额成交不发终态事件？"事件 = 市场可观察事实"这个口径排除了哪些设计？
   - 为什么 `Event` 的每个字段必须有默认值？（W3 差分抓过一个未初始化的 `reason`，复述那次事故。）
   - 一笔 IOC 连续撞上自己三笔同价挂单（CancelOldest），事件流长什么样？outcome 是什么？

**③ 理想效果**

```
[  PASSED  ]  36 tests    ← MatchingW4.*（28）+ EventBusW4.*（8），全绿
```

**④ 答案**：无（口试题的答案就是你对代码的理解）。

---

## 任务 7 ｜ 内存/UB 门禁 + 差分对拍全绿 + 提交

**① 提示**

1. 先跑差分对拍（真实引擎 vs 带账户/STP/事件的慢速参考引擎，
   40 个种子 × 400 步随机 submit/cancel，奇数种子 CancelNewest、偶数种子 CancelOldest）：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/tests/maelstrom_tests --gtest_filter='Seeds/MatchingDifferentialW4.*'
```

2. 再跑内存/UB 门禁（脚本会连 W1-W3 一起编）：

```bash
scripts/run_sanitizers.sh                    # 全部（227 个）
scripts/run_sanitizers.sh 'MatchingW4.*'     # 只跑 W4 语义 + 生命周期
scripts/run_sanitizers.sh 'MatchingW4.Lifetime*'
```

3. 常见红与病因：
   - 差分"事件数不一致" → 漏发/多发发布点（最常见：Market/IOC 余量取消忘了发；
     STP 砍单后又走了一遍取消发布）。
   - 差分"第 k 个事件不一致" → 字段错位（查 price 只在 Trade/OrderRested 有值）或顺序错。
   - 差分"SubmitResult 不一致" → STP-CN 的余量被挂回了簿（查 can_rest 是否排除 self_killed）、
     或 outcome 手工特判错了（W3 的 outcome 规则自动覆盖 STP，不需要新分支）。
   - 差分"find(id) 不一致" → 余量挂簿没带账户、或撤单没删干净。
   - 单测 EventDefaultsAreZeroed 红 → `Event` 字段缺默认成员初始化。
   - `LifetimeStressAccountsStpPolicyChurn` 红 + ASan 报泄漏 → subscribe 了没退订、
     或 cancel 路径先删后用。

4. **提交**：

```bash
git add -A
git commit -m "W4: event bus + self-match prevention (cancel-newest/cancel-oldest)"
```

**③ 理想效果**

```
# 普通 Debug 构建，全量 227 个（W1+W2+W3 的 151 + W4 的 76）
[==========] 227 tests from 9 test suites ran.
[  PASSED  ] 227 tests.

# 内存/UB 门禁（ASan+UBSan+LSan），同样 227 个
scripts/run_sanitizers.sh
[  PASSED  ] 227 tests.               ← 无 sanitizer 报告 = 内存干净、无 UB
```

**④ 答案**：无（这一步是验收与提交）。

---

## 附录：参考答案

> 顺序是**你写完一个任务 → 对答案 → 合上 → 自己再敲一遍**。
> 实现代码属于 `include/` 和 `src/`，是你手写的主战场；附录只给参考，别整段粘贴。

### A1 — 任务 1：types.h 增补 + order.h 增补

`types.h`（与 `OrderId` 同款，放在它旁边）：

```cpp
class AccountId {
public:
    AccountId() = default;
    explicit AccountId(std::int64_t raw) : raw_(raw) {}
    [[nodiscard]] std::int64_t get() const noexcept { return raw_; }

private:
    std::int64_t raw_ = 0;
};

[[nodiscard]] inline bool operator==(AccountId a, AccountId b) noexcept { return a.get() == b.get(); }
[[nodiscard]] inline bool operator!=(AccountId a, AccountId b) noexcept { return a.get() != b.get(); }
```

`order.h`（`type` 之后、最后一行）：

```cpp
class Order {
public:
    Price     price;
    OrderId   orderId;
    Quantity  quantity;
    Side      side;
    OrderType type = OrderType::Limit;
    AccountId account{0}; // 0 = 未设置账户，永不触发自成交防护
};
```

**为什么**：追加 + 默认值 + 不加构造函数，三条纪律与 W3 任务 1 相同；
`{0}` 比省略默认值更明确——聚合初始化少写字段时走"未设置"而非未定义。

### A2 — 任务 2：matching_engine.h 增补

```cpp
// 自成交防护策略（仅当双方账户相同且非 0 时生效）。
enum class SelfMatchPolicy : std::uint8_t {
    CancelNewest, // 即将吃到自己账户的挂单时：主动单余量全部取消，挂单不动
    CancelOldest, // 即将吃到自己账户的挂单时：将该挂单整单撤销，撮合继续
};

enum class EventKind : std::uint8_t {
    Trade,         // 一笔成交
    OrderRested,   // 余量挂入簿（含部分成交后的余量；全额成交不发）
    OrderCanceled, // 簿上订单被撤 / 余量被取消（含 STP 两种策略的取消）
    OrderRejected, // 订单被拒
};

using SubscriptionId = std::uint64_t;

// 事件。与 kind 无关的字段保持默认值（{0} / None）。
struct Event {
    EventKind           kind;
    maelstrom::OrderId  order_id{0};
    maelstrom::AccountId account{0};
    maelstrom::Side     side = maelstrom::Side::Sell;
    maelstrom::OrderId  counterparty_id{0};
    maelstrom::Price    price{0};
    maelstrom::Quantity quantity{0};
    RejectReason        reason = RejectReason::None;
};

// 多订阅者事件总线：同步分发，按订阅注册顺序逐一回调。
class EventBus {
public:
    using Handler = std::function<void(const Event &)>;

    SubscriptionId subscribe(Handler handler) {
        slots_.push_back(Slot{next_id_, std::move(handler)});
        return next_id_++;
    }

    bool unsubscribe(SubscriptionId id) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].id == id) {
                slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    void publish(const Event &event) {
        for (Slot &slot : slots_) {
            slot.handler(event);
        }
    }

    [[nodiscard]] std::size_t subscriber_count() const { return slots_.size(); }

private:
    struct Slot {
        SubscriptionId id;
        Handler        handler;
    };
    std::vector<Slot> slots_;
    SubscriptionId    next_id_ = 1;
};
```

`MatchingEngine` 增补（成员与转发）：

```cpp
public:
    // 切换自成交防护策略，只影响其后的 submit。
    void set_self_match_policy(SelfMatchPolicy policy);

    // 事件总线转发。W3 的 trade handler 通道与之相互独立。
    [[nodiscard]] SubscriptionId subscribe(EventBus::Handler handler);
    bool unsubscribe(SubscriptionId id);

private:
    EventBus        bus_;
    SelfMatchPolicy policy_ = SelfMatchPolicy::CancelNewest;
```

需要 `#include <vector>`。**为什么订阅号不复用**：调用方持有的 id 若被复用，
退订会误伤别人——单调递增让"id 是否有效"永远无歧义。

### A3 — 任务 3/4：matching_engine.cpp 增量

`submit()` 的增量（校验段的 reject 换成发事件的 lambda；循环插入 STP；余量段加发布）：

```cpp
SubmitResult MatchingEngine::submit(maelstrom::Order o) {
    // 1) 校验（顺序不变；account 不参与校验），每个拒绝发一个 OrderRejected
    auto reject = [&](RejectReason r) {
        Event ev{};
        ev.kind   = EventKind::OrderRejected;
        ev.order_id = o.orderId;
        ev.account  = o.account;
        ev.side     = o.side;
        ev.reason   = r;
        bus_.publish(ev);
        SubmitResult res;
        res.outcome = OrderOutcome::Rejected;
        res.reason  = r;
        return res;
    };
    if (o.quantity.get() <= 0) return reject(RejectReason::InvalidQuantity);
    if (book_.find(o.orderId).has_value()) return reject(RejectReason::DuplicateOrderId);

    const bool is_market = (o.type == OrderType::Market);
    if (!is_market && o.price.get() <= 0) return reject(RejectReason::InvalidPrice);
    // ... POST-ONLY / FOK 预检原样保留（不看账户），走 reject(...)
    // 注意：预检是账户盲——PostOnly 撞自己账户的最优价照样拒。

    std::int64_t remaining   = o.quantity.get();
    std::int64_t filled      = 0;
    std::size_t  trades      = 0;
    bool         self_killed = false;

    while (remaining > 0) {
        const auto best = (opp == Side::Sell) ? book_.best_ask() : book_.best_bid();
        if (!best.has_value()) break;
        if (!is_market && !crosses(o.side, o.price, best->price)) break;

        // STP：执行接触点判定（只看队头这一笔；双方账户相同且非 0）
        if (o.account.get() != 0 && best->account == o.account) {
            if (policy_ == SelfMatchPolicy::CancelNewest) {
                Event ev{};
                ev.kind = EventKind::OrderCanceled;
                ev.order_id = o.orderId;  ev.account = o.account;  ev.side = o.side;
                ev.quantity = Quantity{remaining};
                bus_.publish(ev);
                self_killed = true;
                break;
            }
            Event ev{};
            ev.kind = EventKind::OrderCanceled;
            ev.order_id = best->orderId;  ev.account = best->account;  ev.side = best->side;
            ev.quantity = best->quantity;
            bus_.publish(ev);
            book_.cancel(best->orderId);
            continue; // 撮合继续：同价下一笔可能又是自己的
        }

        // ... 原有的 take / consume_best / Trade 事件 / handler_(t) 原样保留 ...
    }

    const std::int64_t original = o.quantity.get();
    Quantity rest{0};
    const bool can_rest = !self_killed &&
                          (o.type == OrderType::Limit || o.type == OrderType::PostOnly);
    if (remaining > 0 && can_rest) {
        Order r = o;                      // 账户随单携带
        r.quantity = Quantity{remaining};
        book_.add(r);
        rest = Quantity{remaining};
        Event ev{};
        ev.kind = EventKind::OrderRested;
        ev.order_id = r.orderId;  ev.account = r.account;  ev.side = r.side;
        ev.price = r.price;  ev.quantity = rest;
        bus_.publish(ev);
    }
    else if (remaining > 0 && !self_killed) {
        // Market/IOC 余量被取消（STP-CN 的取消已在循环内发过，别发两次）
        Event ev{};
        ev.kind = EventKind::OrderCanceled;
        ev.order_id = o.orderId;  ev.account = o.account;  ev.side = o.side;
        ev.quantity = Quantity{remaining};
        bus_.publish(ev);
    }

    // outcome 组装与 W3 完全相同：STP-CN 自然落 Cancelled，不需要新分支
    // ...
}
```

`cancel()` 增量（先捕获字段，再撤，再发事件——顺序不能反）：

```cpp
bool MatchingEngine::cancel(OrderId id) {
    const auto existing = book_.find(id);
    if (!existing.has_value()) return false;
    book_.cancel(id);
    Event ev{};
    ev.kind     = EventKind::OrderCanceled;
    ev.order_id = id;
    ev.account  = existing->account;
    ev.side     = existing->side;
    ev.quantity = existing->quantity;
    bus_.publish(ev);
    return true;
}
```

**为什么**：
- STP-CN 不需要新的 outcome 分支——W3 的规则（resting==0 且 filled<original → Cancelled）
  自动覆盖它；你要做的只是用 `self_killed` 把余量挡在挂簿之外。
- `else if (remaining > 0 && !self_killed)` 是 Market/IOC 的终态事件发出口；
  `!self_killed` 防止与 STP-CN 的取消事件重复。
- 每轮循环要么撤一笔（簿缩小）、要么吃一笔、要么 break——CancelOldest 的连撤必然终止。

### A4 — 任务 5：CMake 改动

`tests/CMakeLists.txt`（仅此一处）：

```cmake
add_executable(maelstrom_tests
    test_order_book.cpp
    test_order_book_w2.cpp
    test_matching_engine_w3.cpp
    test_matching_engine_w4.cpp)
```

`naive_matching_engine_w4.h` 会被测试源自动 include，无需登记；库目标没有任何新增
（W4 没有新的 .cpp / 头文件）。改完必须重新 `cmake -B build ...` 再构建。

### A5 — 跑差分时怎么读失败信息

差分失败信息自带现场：`seed=... step=... 事件数不一致 fast=... slow=... policy=... type=... side=... acct=...`。
- `policy=0` 是 CancelNewest、`policy=1` 是 CancelOldest——先看策略是不是只红一类；
- 事件数一致但内容不一致时，信息里给 `fast_kind` / `slow_kind`（枚举值：0=Trade、
  1=OrderRested、2=OrderCanceled、3=OrderRejected）；
- 把 seed/step 记下来，在两边各加一段打印（或写个小 main 重放前 step+1 步）即可二分定位。

---

## 附：本周边界（做什么、不做什么）

- **做**：`AccountId` 与订单账户、自成交防护两策略（执行接触点判定）、
  `Event`/`EventBus`（多订阅、退订、注册顺序、同步分发）、引擎七处发布点、
  W3 全量回归、事件流参与随机差分、内存/UB 门禁。
- **不做（留给后续）**：
  - 分发期间订阅/退订、多线程分发与并发安全 → W5（无锁队列 + 线程隔离）；
  - 更多 STP 策略（Decrement / RejectIncoming / CancelBoth）→ 简历扩展模块；
  - `std::variant` 事件、事件持久化/回放、订阅优先级 → 扩展模块；
  - 订单改价触发重撮合 → W4 思考题（提示：改价 = 撤单事件 + 新挂单事件？）；
  - 低延迟分发（内联小函数、静态多播）→ W6/W7。

> 一句话记住 W4 的定位：**W3 的引擎只对调用者负责，W4 的引擎对市场负责——
> 每一件发生的事都如实广播（EventBus），每一件不该发生的事都被拦下（STP），
> 而这一切在 40 个随机种子的对拍下与参考实现逐字节一致。**
