# W3 手写任务单 —— 从订单簿到连续撮合引擎

> 这是一份**练习册**，不是参考手册。W2 你交付了一本 O(1) 的限价订单簿：
> 能按号找单、能撤单改单、能 O(1) 查 best——但它**从不成交**，买卖同价可以共存。
> 撮合还完全不存在。W3 要在簿上长出撮合：让买卖单相互吃，产生成交事件，
> 支持限价/市价/IOC/FOK/POST-ONLY 五种订单，并输出"这笔单最后怎么了"的结果。
>
> 每个任务仍按固定四段组织：
>
> **① 问题** → 你此刻面对的具体困难或现象（先想清楚"为什么要有这一步"）
> **② 提示** → 怎么动手的线索（不是完整答案，卡住再往下读）
> **③ 理想效果** → 完成后应该能看到什么（命令输出 / 测试颜色）
> **④ 答案** → 放在文末附录，编号对应。写不出或写完对照，**别复制粘贴，手打一遍**
>
> 规则重申：所有实现代码由你手写进 `include/` 和 `src/`。`tests/` 里已备好**裁判测试**
> （AI 隔离编写，只做验收、不进你的实现目录）：W3 共 92 个用例，分三层——
> 语义单测 41 个 + 簿原语 7 个（`MatchingW3.*` / `BookPrimitivesW3.*`）、
> 内存与 UB 压力 4 个（`MatchingW3.Lifetime*`）、40 组随机差分对拍
> `Seeds/MatchingDifferential.*`（你的引擎 vs 慢速参考引擎）。
> 语义契约见下方"判据"，你的代码和它必须逐字一致。

---

## 0. 先认识问题：W3 到底在解决什么

W2 交付后，`OrderBook::add` 只做一件事：**挂上去**。三个真问题：

| # | 问题 | 后果 |
|---|---|---|
| 1 | **订单簿不会成交**：买卖同价共存，`best_bid >= best_ask` 是常态 | 这不是交易市场，是一个"报价公告板"。真实撮合要求任何时刻簿上不存在可成交的两笔单（无锁交叉） |
| 2 | **没有订单类型**：`Order` 只有价/量/方向，没有"市价""IOC""FOK""POST-ONLY"的概念 | 市价单要扫到簿空、IOC 要撤余量、FOK 要全成或全不成、POST-ONLY 要拒绝吃单——这些语义无处表达 |
| 3 | **下单结果无人知晓**：`add` 只回 `true/false`，不告诉你成了多少、剩多少、为什么被拒 | 交易所回执（ack/nack、成交量、原因码）是撮合引擎的必备输出；回测/风控都靠它 |

W3 的四步解法：

```
①  给 Order 加 type 字段            → 五种订单语义有地方表达
②  给 OrderBook 加两条撮合原语       → 从对手盘"吃"单（consume_best）+ 预查询可成交量（matchable_quantity）
③  新建 MatchingEngine             → 校验 → 撮合循环 → 余量挂簿/取消 → 输出 SubmitResult，每笔成交回调
④  裁判测试（隔离）                → 语义 + 生命周期/UB + 随机差分，三层验收
```

一句面试话术：**W2 解决"订单怎么高效存"，W3 解决"订单怎么高效成交、成交结果怎么可靠地报告"。**

---

## 1. 判据：测试裁判的规则（先通读，别跳）

下面每一条都被 `tests/test_matching_engine_w3.cpp` 和 `tests/naive_matching_engine.h` 强制。
任何一条没满足，测试就红；红的时候回来对照这里。

### 1.1 类型与字段

- `Order` 新增 `OrderType type = OrderType::Limit;`，**必须是最后一个字段且带默认值**——
  这样 W1/W2 的 `Order{price, id, qty, side}` 四段初始化仍合法，不会连累旧代码。
- `OrderType` 取值：`Limit` / `Market` / `Ioc` / `Fok` / `PostOnly`。
- `OrderOutcome` 取值：`Filled`（全额成交、未挂簿）/ `PartiallyFilled`（部分成交、余量已挂簿）/
  `Rested`（零成交、全额挂簿）/ `Cancelled`（有残余但被取消、未挂簿）/ `Rejected`（未受理、零副作用）。
- `accepted() == (outcome != Rejected)`。

### 1.2 校验（顺序固定）

`submit` 在产生任何副作用**之前**依次校验，先命中先返回：

1. `quantity <= 0` → `Rejected{InvalidQuantity}`；
2. `orderId` 已存在（在簿中）→ `Rejected{DuplicateOrderId}`；
3. **非市价单** `price <= 0` → `Rejected{InvalidPrice}`（`Market` 完全忽略 `price`，允许 0/负数）；
4. `POST-ONLY` 且会立即成交 → `Rejected{PostOnlyWouldCross}`；
5. `FOK` 且可成交量 `< quantity` → `Rejected{FokNotFillable}`。

> 顺序很重要：`(id 已存在, 数量非法)` 必须报 `InvalidQuantity`；`(id 已存在, 价格非法, 数量合法)` 必须报 `DuplicateOrderId`。测试专门盯这两个组合。

### 1.3 撮合（限价/市价共用同一条循环）

- 成交优先级：**价格优先，同价时间优先**（价更优的先吃；同价队列里先到的先吃）。
- 成交价 = **被动方（挂单方）价格**。主动方以自己更好的限价成交，即"价格改善"归主动方。
- 限价交叉判定：主动买 `price >= 被动卖价` 即交叉；主动卖 `price <= 被动买价` 即交叉（**相等也算交叉**）。
- 市价单：忽略限价，一直吃对手盘直到**本单吃完**或**对手盘空**。
- 主动单**不会**和自己方向同侧的单成交，只吃对手侧。

### 1.4 余量处理与 outcome

设 `filled` = 本次成交量，`resting` = 最终挂入簿中的量：

| 类型 | 余量去向 | 关键约束 |
|---|---|---|
| `Limit` | 挂入簿中（队尾） | `filled + resting == quantity` |
| `PostOnly` | 挂入簿中（因为必不成交） | 要么 `Rested`，要么 `Rejected`；永不 `Filled` |
| `Market` | **取消** | `resting == 0`；`filled < quantity` 时 outcome 为 `Cancelled` |
| `Ioc` | **取消** | `resting == 0`；同上 |
| `Fok` | 无余量（全成或全不成） | 成交时 `filled == quantity`；不足则 `Rejected` |

`outcome` 判定（按此顺序）：

```
resting > 0           →  filled > 0 ? PartiallyFilled : Rested
resting == 0 且 filled == quantity  →  Filled
resting == 0 且 filled <  quantity  →  Cancelled（有残余被取消）
```

> 易错点：`Market`/`IOC` 只要有残余（哪怕成交了一半）就是 `Cancelled`，**不是** `Filled`。

### 1.5 成交事件

- 每笔成交调用一次 `set_trade_handler` 注册的回调，参数 `Trade{aggressive_id, passive_id, price, quantity, aggressor_side}`。
- **回调顺序 = 撮合顺序**：多档扫单先报最优价那笔。一笔主动单 sweep 3 档 → 3 次回调，顺序即吃单顺序。
- 未设置 handler 时照常撮合，只是不回调；`SubmitResult.trade_count` 仍然准确。
- 回调里**不要**去改引擎状态（重入会破坏当前撮合循环）；只允许读/记录。这是 W4 EventBus 的雏形。

### 1.6 簿侧新增原语（OrderBook 的两个公共方法）

- `consume_best(side, qty)`：从 `side` 侧**最优价位队头**吃掉 `min(qty, 队头剩余量)`。
  - 队头吃光 → 整单移除（空价位删档）；否则队头**减量、保留队首时间优先级**。
  - 返回 `Fill{passive_id, price(该档价), quantity(实吃量)}`；`side` 侧为空或 `qty <= 0` → `nullopt`。
- `matchable_quantity(side, limit)`：`side` 侧**限价内**挂单总量（闭区间）。
  - `side==Buy` 统计 `price >= limit`；`side==Sell` 统计 `price <= limit`；`limit == nullopt` → 该侧全部挂单量（市价单用）。
- `MatchingEngine::cancel(id)` 直接委托 `OrderBook::cancel`；撤单**不产生**成交事件。

### 1.7 其他

- `MatchingEngine` 内部持有 `OrderBook`（值成员或 `unique_ptr` 均可），对外只暴露 `book() const`。
- `MatchingEngine` 不可拷贝、可移动（`OrderBook` 本身已不可拷贝）。
- 主动单被完全成交或拒绝时，其 `orderId` **不入簿**，之后可复用。
- 部分成交的主动单，剩余量以**主动单的 orderId** 挂入簿中（不是换新 id）。

---

## 2. 任务概览（做完一个打一个勾）

| # | 任务 | 改哪个文件 | 预计 |
|---|---|---|---|
| 1 | 给 `Order` 装 `OrderType`（保持旧初始化兼容） | `include/Maelstrom/order.h` | 20 min |
| 2 | 给订单簿加两条撮合原语（W3 最难点） | `include/Maelstrom/order_book.h` + `src/order_book.cpp` | 2–3 h |
| 3 | 定义撮合引擎的对外契约（头文件） | `include/Maelstrom/matching_engine.h` | 1 h |
| 4 | 写引擎实现：校验 → 撮合循环 → 余量 | `src/matching_engine.cpp` | 3–4 h |
| 5 | 把裁判挂进构建，让红灯告诉你缺什么 | `tests/CMakeLists.txt` | 15 min |
| 6 | 语义单测全绿 + 口试自检 | 只读不写 | 1–2 h |
| 7 | 内存/UB 门禁 + 差分对拍全绿 + 提交 | 只读不写 | 1–2 h |

---

## 任务 1 ｜ 给 `Order` 装上订单类型

**① 问题**

W2 的 `Order` 只有四个字段：价、号、量、方向。现在你要表达"市价单/ IOC / FOK / POST-ONLY"，
但这些语义在订单里**没有落脚点**。如果直接往 `Order` 里塞一个新字段且不给默认值，
`Order{Price{...}, OrderId{...}, Quantity{...}, Side::...}` 这种四段初始化会立刻编译失败——
W1/W2 的代码、你现在所有的测试都会一起炸。你得让新字段**既能表达类型，又不破坏旧写法**。

**② 提示**

1. 在 `order.h` 的 `Side` 枚举下面加 `enum class OrderType : std::int8_t { Limit, Market, Ioc, Fok, PostOnly };`。
   为什么要 `: std::int8_t`？和 `Side` 保持一致：枚举只占 1 字节，`Order` 结构体不因为多一个字段而膨胀太多（W6 会关心 cache 占用）。
2. 在 `class Order` 里**最后一行**加 `OrderType type = OrderType::Limit;`。
   - **放最后**：聚合初始化按声明顺序，旧代码只给前四个字段，第五个用默认值。
   - **给默认值**：`Limit` 是最常用、语义最温和的类型；老代码默认限价，行为不变。
3. 别在这里加构造函数。`Order` 目前是聚合类型，`Order{...}` 靠聚合初始化；一旦你写了任何构造函数，它就**不再是聚合**，四段初始化反而编不过。保持它是简单的字段集合。

**③ 理想效果**

```bash
cmake --build build -j        # 成功
ctest --test-dir build         # W1/W2 共 59 个测试仍全绿 —— 你没破坏任何东西
```

改完后你在心里默念一遍：**"加字段 = 追加 + 默认值，绝不插在中间、绝不给聚合类型加构造函数。"**

**④ 答案**：附录 A1。

---

## 任务 2 ｜ 给订单簿加两条撮合原语（本周最难点，值得慢）

**① 问题**

你会本能地想"直接把撮合逻辑写进 `MatchingEngine`，让它自己去掏 `OrderBook` 的链表头、改节点的量"。
但 W2 的 `OrderBook` 用了 pImpl：`Level`、`OrderNode`、`Impl` 全在 `src/order_book.cpp` 里，**外部根本看不见**。
`MatchingEngine` 既拿不到 `Level::head`，也不能安全地"队头减量"。硬做就得把订单簿的内脏全暴露出去，
W2 辛苦封装的边界一夜回到解放前。

正确做法：**由订单簿自己提供一个"从对手盘吃单"的安全动作**。引擎说"我要在卖盘吃 5 手"，
簿负责挑出最优价队头、吃 5 手、维护链表/累计量/空档删除，然后**只把成交明细（被动单 id、价、量）交出来**。
引擎完全不需要知道 `OrderNode` 长什么样。这就叫**封装**：把"改内部结构"的能力留在内部，
对外只暴露一个语义清晰的操作。

**② 提示**

先设计再动手。你要新增两个公共方法（签名见 1.6，附录 A2 有全文）：

1. **`consume_best(Side side, Quantity qty)`**——"从最优价位队头吃一点"。它必须自己处理**两种收尾**：
   - **吃光队头**（`take >= 队头量`）：等价于对这笔挂单做一次 `cancel`。W2 的 `cancel` 已经能"摘链 + 删空档 + 从 id 表删除 + 释放节点"——**复用 `cancel(id)` 就是最省事、最不容易错的做法**（你甚至不用碰 `Level::remove`）。
   - **吃不完队头**（`take < 队头量`）：只需给队头**减量**。注意 W2 的 `OrderNode::modify(newQuant)` 会同步 `Level::totQuant`，直接调它即可；**千万不要**在这里改链表指针，否则时间优先级就没了。
   - 返回 `std::optional<Fill>`；挑档时买卖两侧的 `std::map` 类型不同（W2 的老问题），还是要 if/else 分派或模板化。
2. **`matchable_quantity(Side side, std::optional<Price> limit)`**——"限价内能成交多少"。
   - 这一步**只查不改**。买盘 map 按价格**降序**（`begin()` 是最高买价），所以要 `break` 的条件是 `price < limit`；卖盘升序，`break` 条件是 `price > limit`。
   - 每个价位累加 `Level::totQuant`（W2 已经为 O(1) 查档量准备好了）。**必须靠 `break` 提前停**，别全扫——虽然 FOK 对拍只在簿小的时候跑，但你要养成"只遍历可成交前缀"的习惯，这是 W7 的性能伏笔。
   - `limit == nullopt` 表示不设限（市价单）：整侧全算。

**③ 理想效果**

```bash
cmake --build build -j        # 库能编过
./build/tests/maelstrom_tests --gtest_filter='OrderBookW2.*'   # W2 的 18 个仍全绿
```

> 注意：裁判测试文件同时覆盖引擎（`#include "Maelstrom/matching_engine.h"`），
> 所以 `BookPrimitivesW3.*` 要等你做完任务 3、任务 5 才能真正跑起来。
> 这一步先靠"库编过 + W2 不回归"确认原语没写坏，等引擎就位后再用 7 个原语用例精查。

做完后你能回答：**"`consume_best` 里我复用了 `cancel` 吗？减量时我动了链表指针吗？"**
前者是"是"，后者是"没有"——这就是全周最关键的两个 yes/no。

**④ 答案**：附录 A2。

---

## 任务 3 ｜ 定义撮合引擎的对外契约（头文件先行）

**① 问题**

撮合引擎要返回的东西比 W2 复杂得多：成了多少（`filled`）、剩多少挂簿（`resting`）、
为什么被拒（原因码）、产生了几笔成交（`trade_count`）——`bool` 装不下。
同时"订单类型"和"结果类型"是两个不同的维度，混在一个枚举里会很快失控。
你需要先把**类型系统**设计对，再写实现；否则会被自己的返回值结构反复返工。

**② 提示**

新建 `include/Maelstrom/matching_engine.h`，把下面这几样定清楚（附录 A3 有全文）：

1. `OrderOutcome` / `RejectReason` 两个 `enum class : std::uint8_t`（见 1.1、1.2）。
   - 为什么分开？`outcome` 描述"最终状态"，`reason` 描述"为什么被拒"。只有 `Rejected` 时 `reason` 才有意义，其余时候 `reason == None`。
2. `Trade` 结构：`{aggressive_id, passive_id, price, quantity, aggressor_side}`。
   - 为什么叫 aggressive/passive 而不是 buy/sell？因为成交的两方**一个主动吃、一个被动挂**，价格由被动方决定。用主动/被动命名，语义比用买卖方向更准确（买卖方向由 `aggressor_side` 单独给出）。
3. `SubmitResult`：`{outcome, reason, filled, resting, trade_count}` + `accepted()`。
   - `accepted()` 用 `outcome != Rejected` 一句话实现，调用方读起来比比较枚举舒服。
4. `class MatchingEngine`：
   - `using TradeHandler = std::function<void(const Trade&)>;`
   - `set_trade_handler(TradeHandler)`；`submit(Order)` 标 `[[nodiscard]]`；`cancel(OrderId)`；`book() const noexcept`。
   - 拷贝 `= delete`、移动 `= default`（`OrderBook` 已不可拷贝，这是自然结果）。
   - 内部成员先写 `OrderBook book_;` 和 `TradeHandler handler_;`。这里**不需要再套一层 pImpl**——引擎的成员都是类型完整的，没有 W2 那个"不完整类型析构"的坑。

**③ 理想效果**

头文件写完，`cmake --build build -j` 会因为 `src/matching_engine.cpp` 尚未存在/未登记而报错——
**这是预期的红**。你现在应该能口头背出：`submit` 返回什么、`Trade` 有哪些字段、五种 `OrderType` 各自的余量去哪。

**④ 答案**：附录 A3。

---

## 任务 4 ｜ 写引擎实现：校验 → 撮合循环 → 余量

**① 问题**

这是 W3 的核心：把"五种订单类型"统一到**同一条撮合循环**里。
难点在于，如果为每种类型各写一套循环，代码会迅速膨胀成五份互相抄的实现，
后期改一个成交价规则要改五处。你要找到它们的**共性**，把差异收进"校验"和"余量处理"两端，
中间那段"吃单"对限价和市价是同一件事。

**② 提示**

新建 `src/matching_engine.cpp`，按固定顺序写（附录 A4 是参考实现）：

```
submit(o):
  1. 校验（严格按 1.2 的顺序，先数量 -> 再重复 id -> 再价格）
  2. POST-ONLY 预测：拿对手侧 best，若交叉 -> 拒绝
  3. FOK 预测：matchable_quantity(对手侧, limit) < 需求量 -> 拒绝
  4. 撮合循环：while (remaining > 0) { 取对手 best; 不交叉/Market 则 break;
                 take = min(remaining, best 量); consume_best(对手侧, take);
                 发 Trade 事件; 更新 filled/remaining/trades; }
  5. 余量：Limit/PostOnly -> book_.add(剩余); 其余丢弃（即取消）
  6. 组装 SubmitResult（outcome 按 1.4 判定）
```

几个必须想清楚的点：

- **`is_market` 是一个布尔开关**，撮合循环里唯一的区别是"要不要检查交叉"。把 `limit` 表示为 `std::optional<Price>`：
  Market 传 `nullopt`，其余传 `o.price`。这样交叉判定和 `matchable_quantity` 都能共用同一段代码。
- **成交价一律取 `fill->price`（被动方价格）**，绝不能用 `o.price`（那是主动方限价）。这是对手价撮合的核心。裁判测试里有一个专门的突变场景盯这条（用主动价会立刻爆一片红）。
- **`consume_best` 返回的 `quantity` 可能小于你请求的 `take`**（理论上不会，因为 `take <= best 量`）。稳妥起见用返回值更新 `remaining`，而不是用 `take`。
- **事件时机**：在每一轮吃单**之后**立刻回调一次，参数用刚返回的 `fill`。别攒到最后一起发——`Trade` 的顺序就是市场数据里成交明细的顺序。
- **余量挂簿前要改 `quantity`**：`Order r = o; r.quantity = Quantity{remaining}; book_.add(r);`。挂进去的必须是**剩余量**，不是原始量；同时 `orderId` 保持主动单的 id（1.7 最后一条）。
- **FOK 校验用的 `matchable_quantity` 要和撮合循环用同一套交叉规则**：都用"主动方视角"。写反了（比如买用 `>=` 写成了 `<=`）会让 FOK 预测和实际成交对不上，差分对拍会抓到。

**③ 理想效果**

```bash
cmake --build build -j
./build/tests/maelstrom_tests --gtest_filter='MatchingW3.*'    # 45 个语义 + 生命周期用例，分批判红转绿
```

建议按类型分批：先让 `Limit` 相关绿 → 再 `Market` → 再 `IOC` → 再 `FOK` → 最后 `POST-ONLY`。
`POST-ONLY` 的 `PostOnlyWouldCross` 判定最容易写错成"严格大于"，记住**相等也要拒**（1.2 第 4 条）。

*常见红与病因：*

- [ ] `outcome` 全判成 `Filled` → 忘了 `resting` 分支（Market/IOC 残余应为 `Cancelled`）。
- [ ] 成交价用了主动价 → `LimitBuyFullCrossUsesPassivePrice` 等一批红。
- [ ] 限定单没判 `empty()` 就取 `best` → 空簿市价单崩溃（`MarketOnEmptyBookCancels`）。
- [ ] FOK 校验条件写反/用错 side → `FokNotFillableRejectedWithoutSideEffects` 红。
- [ ] POST-ONLY 相等价没拒 → `PostOnlyRejectsWhenWouldCrossAtEqualPrice` 红。
- [ ] 余量挂的是原始量而非剩余量 → `BookReflectsRestingRemainderUnderAggressorId` 红。
- [ ] `trade_count` 用了 `filled` 笔数而非**成交笔数**（一笔可能吃多个被动单）→ 计数对不上。

**④ 答案**：附录 A4。

---

## 任务 5 ｜ 把裁判挂进构建，把红灯读成 TODO

**① 问题**

裁判测试已经写好了，但构建系统不知道它的存在；不改 CMake，你写半天也没人告诉你对没对。

**② 提示**

1. 打开 `tests/CMakeLists.txt`，把新测试源加进 `add_executable` 的源列表（和 W2 时一样，**只改这一行**）：
   `naive_matching_engine.h` 是头文件，被测试源 `#include`，**不需要**写进 CMake。
2. 把引擎实现登记进库目标：打开根 `CMakeLists.txt`，在 `add_library(maelstrom STATIC ...)` 的源列表里加上
   `src/matching_engine.cpp` 和 `include/Maelstrom/matching_engine.h`（和现有 `order_book.cpp` 并列）。
3. 改了 CMake 必须重新配置再构建：

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DMAELSTROM_BUILD_TESTS=ON -DMAELSTROM_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

4. 认一下过滤尺子：

```bash
./build/tests/maelstrom_tests --gtest_filter='MatchingW3.*'          # 语义 + 生命周期
./build/tests/maelstrom_tests --gtest_filter='BookPrimitivesW3.*'    # 簿原语
./build/tests/maelstrom_tests --gtest_filter='Seeds/MatchingDifferential.RandomSubmitSequence/7'
./build/tests/maelstrom_tests --gtest_filter='MatchingW3.Fok*'       # FOK 一族
ctest --test-dir build --output-on-failure                           # 全量（含 W1/W2）
```

**③ 理想效果**

你能说出："W3 要我交付的 = `Order.type`、`consume_best`、`matchable_quantity`、`MatchingEngine`
（`submit` + 五个类型语义 + 事件回调 + `cancel`）。" 构建目前红着——**预期内的红**，是起跑线。

**④ 答案**：附录 A5（两处改动，各一行）。

---

## 任务 6 ｜ 语义单测全绿 + 口试自检

**① 问题**

单测绿了只能说明"精心构造的场景"对了。但这一周你要能对着一张白纸，
把"为什么这么设计"讲清楚——面试不会让你默写代码，它会问你取舍。

**② 提示**

1. 跑全量语义：

```bash
./build/tests/maelstrom_tests --gtest_filter='MatchingW3.*:BookPrimitivesW3.*'
```

2. 失败时按类型缩小范围（`Fok*` / `Ioc*` / `PostOnly*` / `Market*` / `Lifetime*`）。
   差分失败会告诉你 seed / step / 哪个观察量不一致。
3. **口试自检**（不用写代码，但要能对着代码讲，写进笔记）：
   - 为什么 `Trade` 用 aggressive/passive 命名，而不是 buy/sell？成交价为什么必须是被动价？
   - 限价和市价为什么能共用一条撮合循环？`std::optional<Price>` 在其中扮演什么角色？
   - `POST-ONLY` 的判定为什么用"best 对手价是否交叉"，而不是"能不能成交"？相等价为什么要拒？
   - `FOK` 的预校验和实际撮合为什么必须用**同一套**交叉规则？如果两者不一致会怎样（提示：看差分测试的观察量）？
   - `PartiallyFilled` 和 `Cancelled` 的区别是什么？同样"没成交完"，为什么市价单是 `Cancelled` 而限价单是 `PartiallyFilled`？
   - 一笔主动单 sweep 三个价位，`Trade` 回调的**顺序**由什么决定？谁保证它？

**③ 理想效果**

```
[  PASSED  ]  52 tests    ← MatchingW3.*（45）+ BookPrimitivesW3.*（7），全绿
```

**④ 答案**：无（口试题的答案就是你对代码的理解）。

---

## 任务 7 ｜ 内存/UB 门禁 + 差分对拍全绿 + 提交

**① 问题**

撮合路径的每个 bug 都可能变成内存错误：`consume_best` 吃光队头时若先删节点再删空档，就是
**use-after-free**；部分成交改量若漏同步 `totQuant`，会是逻辑错；主动单余量若重复挂入，
是内存泄漏或 id 冲突。普通单测**可能通过**，但内存已经烂了。
所以 W3 除了语义单测，还专门要求**三层验收**：语义、随机差分、以及 ASan/UBSan/LSan。

**② 提示**

1. 先跑差分对拍（默认引擎 vs 慢速参考引擎，40 个种子 × 400 步随机 submit/cancel）：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/tests/maelstrom_tests --gtest_filter='Seeds/MatchingDifferential.*'
```

2. 再跑**内存/UB 门禁**。项目里有一个隔离脚本，它用 ASan + UBSan + LSan 重新编译库和全部测试并运行：

```bash
scripts/run_sanitizers.sh                    # 全部（W1+W2+W3）
scripts/run_sanitizers.sh 'MatchingW3.*'     # 只跑 W3 语义 + 生命周期
scripts/run_sanitizers.sh 'MatchingW3.Lifetime*'   # 只跑压力用例
```

   脚本不改你的 CMake、不碰你的 `include/` `src/`，只读源码另编一份到 `/tmp`——对你的手写实现零干扰。

3. 三类报告的读法：
   - `AddressSanitizer: heap-use-after-free` → 你在 `consume_best` 里先用了节点/档，后释放；检查"摘链 → 删空档 → 删 id → delete"的顺序（W2 任务 4 讲过的铁律）。
   - `AddressSanitizer: leaked ...` → 有 `new` 出来的节点没被析构；或 `MatchingEngine` 移动后源对象留下裸指针。
   - `runtime error: signed integer overflow` → `Quantity` 加减溢出；确认你用的是 `std::int64_t` 且没有隐式缩窄成 `size_t`。
   - 差分 `SubmitResult 不一致` → 校验顺序 / outcome 判定 / 余量处理与判据对不上。
   - 差分 `第 k 笔成交不一致` → 成交价、成交量或撮合顺序错（优先查被动价与 price-time）。

4. **压力用例**（`MatchingW3.Lifetime*`）故意制造大量建簿/扫簿/撤单/析构，
   在 ASan 下跑它们，是抓 use-after-free 和泄漏最有效的手段。普通 Debug 构建下它们跑得很快，可以常跑。

5. **提交**（先确认在 W3 分支上）：

```bash
git add -A
git commit -m "W3: continuous matching engine (limit/market/IOC/FOK/post-only)"
```

**③ 理想效果**

```
# 语义 + 差分（普通 Debug 构建），W3 自身共 92 个
[==========] 92 tests ran.             ← 全 PASSED

# 内存/UB 门禁（ASan+UBSan+LSan），脚本会连 W1/W2 一起编，共 151 个
scripts/run_sanitizers.sh
[==========] 151 tests from 6 test suites ran.
[  PASSED  ] 151 tests.               ← 无 sanitizer 报告 = 内存干净、无 UB
```

**④ 答案**：无（这一步是验收与提交）。

---

## 附录：参考答案

> 顺序是**你写完一个任务 → 对答案 → 合上 → 自己再敲一遍**。
> 实现代码属于 `include/` 和 `src/`，是你手写的主战场；附录只给参考，别整段粘贴。

### A1 — 任务 1：order.h

```cpp
#pragma once
#include <cstdint>

#include "Maelstrom/types.h"

namespace maelstrom {

enum class Side : std::int8_t {
    Sell,
    Buy
};

// 订单类型。默认 Limit，保证 W2 的 Order{price, id, qty, side} 四段初始化仍生效。
enum class OrderType : std::int8_t {
    Limit,    // 限价：可成交部分立即成交，余量挂簿
    Market,   // 市价：吃对手盘直到簿空，余量取消（price 忽略）
    Ioc,      // Immediate-Or-Cancel：限价撮合，余量取消
    Fok,      // Fill-Or-Kill：不能全额成交则整单拒绝
    PostOnly, // 只做 Maker：会立即成交则拒绝，否则挂簿
};

class Order {
public:
    Price     price;
    OrderId   orderId;
    Quantity  quantity;
    Side      side;
    OrderType type = OrderType::Limit; // 新字段放最后并带默认值，保持接口兼容
};
} // namespace maelstrom
```

**为什么**：`type` 放最后 + 默认值 = 老的四段初始化仍成立（聚合初始化按顺序填，缺省走默认值）；
不加构造函数是为了让 `Order` 保持聚合类型。这一条是"向后兼容地演进数据结构"的模板，面试常问。

### A2 — 任务 2：order_book.h 增补 + order_book.cpp 实现

**头文件**：在 `class OrderBook` 里（`size()` 之后）加：

```cpp
// consume_best 的返回值：本次实际从对手侧吃掉的量、成交价、被动单 id。
struct Fill {
    OrderId  passive_id;
    Price    price;
    Quantity quantity;
};

// 从 side 侧最优价位队头吃掉 min(qty, 队头剩余量)：队头吃光则整单移除，
// 否则同价减量、保留队首时间优先级。返回成交明细；该侧为空或 qty<=0 返回 nullopt。
[[nodiscard]] std::optional<Fill> consume_best(Side side, Quantity qty);

// side 侧在 limit 内的挂单总量（闭区间）：
//   side==Buy  统计 price >= limit 的量；side==Sell 统计 price <= limit 的量。
//   limit 为空 -> 该侧全部挂单量（市价单用）。
[[nodiscard]] Quantity matchable_quantity(Side side, std::optional<Price> limit) const;
```

**实现**（`src/order_book.cpp`，放在其它方法之后、命名空间收尾之前）：

```cpp
std::optional<Fill> OrderBook::consume_best(Side side, Quantity qty) {
    if (!impl || qty.get() <= 0) {
        return std::nullopt;
    }
    auto &bid    = impl->orders_bid;
    auto &ask    = impl->orders_ask;
    Level *level = nullptr;
    Price  price{0};
    if (side == Side::Buy) {
        if (bid.empty()) {
            return std::nullopt;
        }
        auto it = bid.begin();
        price   = it->first;
        level   = &it->second;
    }
    else {
        if (ask.empty()) {
            return std::nullopt;
        }
        auto it = ask.begin();
        price   = it->first;
        level   = &it->second;
    }

    OrderNode         *node  = level->head;
    const OrderId      pid   = node->order.orderId;
    const std::int64_t avail = node->order.quantity.get();
    const std::int64_t take  = std::min(qty.get(), avail);

    if (take >= avail) {         // 队头被吃光：整单移除
        impl->orders.erase(pid); // 析构 OrderNode -> level->remove(this)
        if (level->size == 0U) { // 该档空了：删档
            if (side == Side::Buy) {
                bid.erase(price);
            }
            else {
                ask.erase(price);
            }
        }
    }
    else {                       // 部分成交：队头减量，队列位置不变
        node->modify(Quantity{avail - take});
    }
    return Fill{pid, price, Quantity{take}};
}

Quantity OrderBook::matchable_quantity(Side side, std::optional<Price> limit) const {
    if (!impl) {
        return Quantity{0};
    }
    std::int64_t sum = 0;
    if (side == Side::Buy) { // 买盘 map 按价格降序
        for (const auto &[price, level] : impl->orders_bid) {
            if (limit.has_value() && price.get() < limit->get()) {
                break;
            }
            sum += level.totQuant.get();
        }
    }
    else { // 卖盘 map 按价格升序
        for (const auto &[price, level] : impl->orders_ask) {
            if (limit.has_value() && price.get() > limit->get()) {
                break;
            }
            sum += level.totQuant.get();
        }
    }
    return Quantity{sum};
}
```

**为什么**：`consume_best` 两步收尾都**复用 W2 已有的机制**——吃光走 `orders.erase(pid)`
（节点析构自动 `level->remove(this)`），减量走 `node->modify(新量)`（自动同步 `totQuant`），
全程没手写链表指针，W2 的正确性直接继承过来。`std::min` 保证不会吃掉下一笔单。

> ⚠️ 若你的 W2 没有"节点析构自动摘链"这条设计（比如节点是裸 `new/delete` 手工管理），
> 那么吃光队头时要显式走你自己的 `remove(node)` 路径，保证"摘链 → 空档删除 → 删 id → 释放"顺序不变。

### A3 — 任务 3：matching_engine.h

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Maelstrom/order_book.h"

namespace maelstrom {

// 订单最终去向。accepted == (outcome != Rejected)。
enum class OrderOutcome : std::uint8_t {
    Filled,          // 完全成交，无剩余，未挂簿
    PartiallyFilled, // 部分成交，剩余已挂入簿中
    Rested,          // 零成交，全额挂入簿中
    Cancelled,       // 已受理但剩余被取消（Market/IOC 余量），簿中无剩余
    Rejected,        // 未受理，簿与成交均无任何副作用
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidPrice,       // 限价类订单 price <= 0
    InvalidQuantity,    // quantity <= 0
    DuplicateOrderId,   // orderId 已在簿中
    PostOnlyWouldCross, // POST-ONLY 会立即成交
    FokNotFillable,     // FOK 无法全额成交
};

// 一笔成交事件。price 为被动方（挂单方）价格，aggressor 吃到价格改善。
struct Trade {
    OrderId  aggressive_id;
    OrderId  passive_id;
    Price    price;
    Quantity quantity;
    Side     aggressor_side;
};

struct SubmitResult {
    OrderOutcome outcome = OrderOutcome::Rejected;
    RejectReason reason  = RejectReason::None; // 仅 outcome == Rejected 时有意义
    Quantity     filled{0};                    // 本次成交总量
    Quantity     resting{0};                   // 最终留在簿上的剩余量
    std::size_t  trade_count = 0;              // 本次产生的 Trade 笔数

    [[nodiscard]] bool accepted() const noexcept { return outcome != OrderOutcome::Rejected; }
};

// 撮合引擎：单线程、price-time priority，内部持有一本 OrderBook。
class MatchingEngine {
public:
    using TradeHandler = std::function<void(const Trade &)>;

    MatchingEngine()  = default;
    ~MatchingEngine() = default;

    MatchingEngine(const MatchingEngine &)                = delete;
    MatchingEngine &operator=(const MatchingEngine &)     = delete;
    MatchingEngine(MatchingEngine &&) noexcept            = default;
    MatchingEngine &operator=(MatchingEngine &&) noexcept = default;

    // 每产生一笔成交就回调一次（未设置则只统计、不回调）。
    void set_trade_handler(TradeHandler handler);

    // 提交一笔订单，返回本次处理结果。所有副作用（成交、挂簿）在返回前完成。
    [[nodiscard]] SubmitResult submit(Order o);

    // 撤单：委托给内部订单簿；不在簿返回 false。
    bool cancel(OrderId id);

    [[nodiscard]] const OrderBook &book() const noexcept { return book_; }

private:
    OrderBook    book_;
    TradeHandler handler_;
};

} // namespace maelstrom
```

**为什么**：`SubmitResult` 四件套（outcome / reason / filled / resting）把"结果"和"原因"分开，
`trade_count` 让调用方不做事件回调也能知道成交笔数。`book()` 返回 const 引用而非非 const，
是刻意的只读边界——外部不能绕过引擎直接改簿，否则撮合不变量（无锁交叉）会被破坏。

### A4 — 任务 4：matching_engine.cpp

```cpp
#include "Maelstrom/matching_engine.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace maelstrom {

void MatchingEngine::set_trade_handler(TradeHandler handler) { handler_ = std::move(handler); }

bool MatchingEngine::cancel(OrderId id) { return book_.cancel(id); }

namespace {

SubmitResult rejected(RejectReason r) {
    SubmitResult res;
    res.outcome = OrderOutcome::Rejected;
    res.reason  = r;
    return res;
}

// 主动方视角：被动价 passive 是否与主动方限价 limit 交叉。
bool crosses(Side aggressor, Price limit, Price passive) {
    return aggressor == Side::Buy ? (passive.get() <= limit.get()) : (passive.get() >= limit.get());
}

} // namespace

SubmitResult MatchingEngine::submit(Order o) {
    // 1) 校验（顺序固定：数量 -> 重复 id -> 价格）
    if (o.quantity.get() <= 0) return rejected(RejectReason::InvalidQuantity);
    if (book_.find(o.orderId).has_value()) return rejected(RejectReason::DuplicateOrderId);

    const bool is_market = (o.type == OrderType::Market);
    if (!is_market && o.price.get() <= 0) return rejected(RejectReason::InvalidPrice);

    const Side opp = (o.side == Side::Buy) ? Side::Sell : Side::Buy;

    // 2) POST-ONLY：会立即成交就拒（无副作用）
    if (o.type == OrderType::PostOnly) {
        const auto best = (o.side == Side::Buy) ? book_.best_ask() : book_.best_bid();
        if (best.has_value() && crosses(o.side, o.price, best->price)) {
            return rejected(RejectReason::PostOnlyWouldCross);
        }
    }

    // 3) FOK：可成交量不足则整单拒绝（无副作用）
    const std::optional<Price> limit = is_market ? std::nullopt : std::optional<Price>{o.price};
    if (o.type == OrderType::Fok) {
        if (book_.matchable_quantity(opp, limit).get() < o.quantity.get()) {
            return rejected(RejectReason::FokNotFillable);
        }
    }

    // 4) 连续撮合：吃对手侧最优价位队头，直到不交叉或本单吃完
    std::int64_t remaining = o.quantity.get();
    std::int64_t filled    = 0;
    std::size_t  trades    = 0;

    while (remaining > 0) {
        const auto best = (opp == Side::Sell) ? book_.best_ask() : book_.best_bid();
        if (!best.has_value()) break;                        // 对手盘空
        if (!is_market && !crosses(o.side, o.price, best->price)) break; // 不再交叉

        const std::int64_t take = std::min(remaining, best->quantity.get());
        const auto         fill = book_.consume_best(opp, Quantity{take});
        if (!fill.has_value()) break; // 契约保证不会发生

        const Trade t{o.orderId, fill->passive_id, fill->price, fill->quantity, o.side};
        if (handler_) handler_(t);

        filled += fill->quantity.get();
        remaining -= fill->quantity.get();
        ++trades;
    }

    // 5) 余量：Limit/PostOnly 挂簿，其余（Market/IOC 的残余）取消
    const std::int64_t original = o.quantity.get();
    Quantity           rest{0};
    const bool         can_rest = (o.type == OrderType::Limit || o.type == OrderType::PostOnly);
    if (remaining > 0 && can_rest) {
        Order r    = o;
        r.quantity = Quantity{remaining};
        book_.add(r);
        rest = Quantity{remaining};
    }

    SubmitResult res;
    res.filled      = Quantity{filled};
    res.resting     = rest;
    res.trade_count = trades;
    if (rest.get() > 0) {
        res.outcome = (filled > 0) ? OrderOutcome::PartiallyFilled : OrderOutcome::Rested;
    } else if (filled == original) {
        res.outcome = OrderOutcome::Filled;    // 全额成交
    } else {
        res.outcome = OrderOutcome::Cancelled; // 有残余但被取消，未挂簿
    }
    return res;
}

} // namespace maelstrom
```

**为什么**：
- 校验在最前面，保证任何 `Rejected` 路径都**零副作用**——这是交易所语义，也是对拍测试不停比状态的底气。
- `limit` 用 `std::optional` 统一表达"限价/不限价"，让限价与市价共用同一条循环，差异只剩一个 `!is_market` 判断。
- 成交价取 `fill->price`（被动价）：主动方出 105 买、被动方挂 100 卖，成交在 100，价差归主动方。
- `outcome` 用"有没有 resting / 是否吃满原始量"两步判定，`Market`/`IOC` 的残余自然落入 `Cancelled`。

### A5 — 任务 5：CMake 改动

根 `CMakeLists.txt`：

```cmake
add_library(maelstrom STATIC
    src/order_book.cpp
    src/matching_engine.cpp
    include/Maelstrom/order_book.h
    include/Maelstrom/matching_engine.h)
```

`tests/CMakeLists.txt`：

```cmake
add_executable(maelstrom_tests
    test_order_book.cpp
    test_order_book_w2.cpp
    test_matching_engine_w3.cpp)
```

`naive_matching_engine.h` 会被测试源自动 `#include`，无需登记。改完必须重新 `cmake -B build ...` 再 `cmake --build build -j`。

---

## 附：本周边界（做什么、不做什么）

- **做**：五种订单类型、连续撮合、price-time 优先级、成交事件回调、`SubmitResult` 结果码、
  `consume_best`/`matchable_quantity` 两条簿原语、内存/UB 门禁。
- **不做（留给后续）**：
  - 自成交防护（self-trade prevention）→ W4；
  - 多订阅者 EventBus、事件持久化/回放 → W4；
  - 订单改价触发重撮合（W2 的 `modify` 只是挂单簿操作，不撮合）→ 可作为 W4 思考题；
  - 无锁 SPSC 队列与多线程 → W5；
  - 低延迟布局与 benchmark → W6/W7。

> 一句话记住 W3 的定位：**W2 把订单存得又快又稳，W3 让它们按市场规则成交，并把"成交了什么、还剩什么、为什么没成"如实回答给调用者。**
