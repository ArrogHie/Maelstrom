# W4 手写任务单 —— 让引擎学会广播（EventBus）与自律（自成交防护）

> 这是一份**练习册**，不是参考手册。W3 你交付的引擎会成交、会回执，但它有两个"单点故障"：
> ① 成交只告诉一个人（`set_trade_handler` 是单槽位），撤单和拒单干脆没有出口；
> ② 它会自己吃自己（同一账户的双边挂单照常成交）。
> W4 给引擎装上"广播器"与"自律机制"，并用与 W3 同级的四层验收把它们锁死。
>
> 与 W3 相同的四段任务格式和外置裁判规则不变。但这份文档的**读法**有一处不同：
> 类型与字段不再以清单形式先给你（那样你只会背名字），而是每一类东西都从
> "现在到底疼在哪"推导出来；推导完才给出正式契约。测试红了的时候，
> 先到文末 **「契约速查」** 对照定位（那里是速查表，不是学习材料），再回 §1/§2 看它为什么长这样。
>
> 规则重申：所有实现代码由你手写进 `include/` 和 `src/`。`tests/` 里已备好**裁判测试**
> （AI 隔离编写，只做验收、不进你的实现目录）：W4 共 84 个用例——
> 语义单测 32 + 生命周期压力 3（`MatchingW4.*`）、簿原语 1（`BookPrimitivesW4.*`）、
> 总线单测 8（`EventBusW4.*`）、40 组随机差分 `Seeds/MatchingDifferentialW4.*`
> （你的引擎 vs 带账户与 STP 的慢速参考引擎，逐步比对 SubmitResult / **完整事件流** / 簿状态）。
>
> **本版修订（FOK × STP）**：初版契约允许 CancelNewest 下的 FOK 被 STP 砍成"部分成交"，
> 违反 FOK 全成或全不成的语义。本版改为 **FOK 预检按"当前策略下的可达流动性"判定**：
> 通过预检 ⇔ 必然全额成交；否则整体拒绝、对簿零副作用（§2.5 有完整推导与例子）。
>
> **回归铁律：W1–W3 的 151 个测试必须保持全绿。** 本周你在给一台正在运转的引擎追加字段和
> 逻辑，旧测试是你唯一的安全网——"新功能绿了但旧的红了"不算任何进展。
>
> 前置状态：W3 已完成、151 全绿。语言标准 C++17。

---

## 0. W3 交付了什么，两个新需求从哪里来

### 0.1 现状盘点

W3 结束时，`MatchingEngine` 对外恰好三件能力，先把它们摆出来（后面所有推导都从这三件能力的边界出发）：

```cpp
SubmitResult submit(Order o);         // 五种订单类型 + 连续撮合，回答"我这一单怎么了"
void set_trade_handler(TradeHandler); // 单个成交回调槽位
bool cancel(OrderId id);              // 撤单，只回一个 bool
```

### 0.2 需求 A：引擎里发生的事，要能同时告诉很多人

场景具体到步骤：你把引擎接进一个交易台。

- **第一天**：日志模块要成交明细 → `set_trade_handler` 挂上，能用。
- **第二天**：风控模块也要成交，还要监控挂单变化和撤单 → 三个麻烦同时冒出来：

| 麻烦 | 现状 | 后果 |
|---|---|---|
| 槽位只有一个 | `set_trade_handler` 是**替换**语义（W3 的 `HandlerCanBeReplaced` 测的就是"换人后旧回调不再收到"） | 风控挂上，日志当场失聪 |
| 撤单没有出口 | `cancel(id)` 只回 `bool`，不产生任何回调 | 风控无法维护"当前还有哪些单在外面" |
| 拒单没有出口 | 被拒的 `submit` 不发任何回调（W3 测试专门断言拒单时回调为空） | 风控看不见被拒流量的原因分布——而那恰是它最想看的 |

三个麻烦指向同一个能力缺口：引擎内部"发生了什么"需要一个**结构化的、可多播的、带身份的**出口。

### 0.3 需求 B：引擎不许自己吃自己

第二个场景：做市策略在同一账户（比如 account=7）同时挂了卖 5@100 和买 10@100。W3 的引擎非常忠实地让它们成交了——5 手被"自己人"吃掉。这在真实市场里的性质：

- **成交量/成交额虚增**：同一主体的买卖对倒不改变任何净头寸，却让成交统计、返佣计算、行情 tape 全部失真——这正是"洗售"（wash trading）的定义；
- **风控口径污染**：持仓、盈亏、敞口全被无意义成交干扰；
- **合规要求**：真实交易所普遍提供自成交防护（Self-Match Prevention, STP）机制，很多场景下同一账户自成交是被明文禁止的，JD 里"自成交防护"也是高频词。

所以第二件事：引擎在执行层面拦下"自己撞自己"，并且把拦截过程变成**可观测事件**（谁被取消了、取消了多少）——需求 A 和 B 天然咬合在一起。

### 0.4 本周验收标准（先定验收，再谈实现）

1. **多播可用**：任意多个观察者可以订阅/退订引擎事件；能收到成交、挂簿、取消、拒绝四类事实；字段完整、顺序确定、同步送达。
2. **STP 生效**：同一账户（非 0）的主动单不会与自己的挂单成交；两种策略（CancelNewest / CancelOldest）逐条符合契约；`account == 0` 的订单行为与 W3 **逐字节一致**。
3. **FOK 的承诺不被打破**：任何策略下，FOK 要么全额成交、要么整体拒绝（按"可达流动性"预检，§2.5）；永不部分成交、永不挂簿；失败时对簿零副作用。
4. **回归与门禁**：W1–W3 的 151 个测试不改一行仍全绿；W4 新增 84 个全绿；ASan/UBSan/LSan 下 235 个全绿。

范围边界（明确不做）：Decrement / RejectIncoming 等更多 STP 策略、异步分发与多线程（W5）、事件持久化/回放、`std::variant` 版事件。这些都留到扩展模块，理由在 §2.3 和 §1.3 里各自说明。

---

## 1. 需求 A 的推导：从单槽位到事件总线

### 1.1 自然的基线方案，与它的三处硬伤

面对 0.2 的麻烦，最顺手的改良是把"一个槽位"换成"一组回调"。这是**合理**的第一反应，不要跳过它直接看答案：

```cpp
// 基线 B1：把槽位扩成数组
std::vector<std::function<void(const Trade &)>> handlers_;
void add_trade_handler(std::function<void(const Trade &)> h) { handlers_.push_back(std::move(h)); }
// submit 循环里：for (auto &h : handlers_) h(t);
```

B1 解决了"多个消费者"的一半问题，但马上碰到三处硬伤：

1. **没有身份，退订只能靠下标**。风控模块想停掉自己的订阅，你只能给它一个 `size_t index`。而 `handlers_.erase(begin()+1)` 之后，原来拿 index=2 的订阅者变成了 index=1——它下次退订会删掉别人。这是一种"句柄随容器搬运而错位"的经典 bug，W2 撤单时你已经在订单簿里见过它的反面（订单簿用 `orderId` 而不是数组下标做句柄，正是为了这个）。
2. **消息太窄**。仍然是成交专用类型。挂簿、撤单、拒绝都没有出口（0.2 的后两行麻烦原封不动）。
3. **事实种类会继续增长**。今天四种，明天可能要加"订单被改价"。如果每加一种事实就在引擎上加一对 `add_xxx_handler/remove_xxx_handler`，API 会被事实清单钉死。

另一种合理的基线是"每种事实一条独立通道"：`on_trade / on_rest / on_cancel / on_reject` 四个回调数组。它类型更漂亮，但**跨通道的顺序丢了**——一个想按真实时间顺序回放全部事件的消费者（回测/日志/行情镜像）必须自己把四条流合并成一条，还要再建一套全局时序。既然"顺序"本身是我们要保证的东西，那就应该由引擎在**一条流**上保证。

于是能力缺口收敛成三条：

| # | 缺口 | 对应设计决定 |
|---|---|---|
| A1 | 一条**覆盖全部事实**的消息类型 | `Event` + `EventKind`（§1.2、§1.3） |
| A2 | 订阅要有**稳定身份**，能精确退订 | `SubscriptionId` + `subscribe/unsubscribe`（§1.4） |
| A3 | **确定的投递顺序与时机** | 注册顺序 + 同步分发（§1.4） |

### 1.2 消息的形状从哪里来：枚举"订单身上能发生的事实"

不要凭空设计消息。"引擎里能发生什么"这件事是可数的——走一遍 `submit` / `cancel` 的全部出口，问每个出口**对外部世界做了什么可观察的变化**：

| 出口 | 外部世界可观察到的变化 |
|---|---|
| 吃掉对手盘队头 | 市场发生了成交 |
| 余量挂入簿中 | 簿上多了一笔挂单 |
| 余量被丢弃（Market/IOC 的残余） | 一笔已被受理的单死了，且簿上什么都没有 |
| 整单被拒 | 没有市场变化，但"拒绝"本身是一个可观察事实 |
| `cancel(id)` 成功 | 簿上少了一笔挂单 |

（最后两个是"簿没变但状态事实新发生"，前三个是"簿变了"。）

这五行就是消息种类的全部来源。把措辞统一到"事实"上：

```cpp
enum class EventKind : std::uint8_t {
    Trade,         // 一笔成交
    OrderRested,   // 余量挂入簿（含部分成交后的余量）
    OrderCanceled, // 簿上订单被撤 / 余量被取消
    OrderRejected, // 订单被拒
};
```

**四个值不是"定的"，是数出来的。** 数法：任何一笔订单的任何结局，都落在"成交了一段 / 挂进了簿 / 被取消了 / 被拒绝了"这四类可观察事实里。以后要加事实（比如"订单被改价"），往这个枚举里加值，总线与订阅端结构不用动——这正是 B1 硬伤 3 想要的性质。

**关键取舍：全额成交为什么没有"完成"事件？** 这不是漏了，是两条输出通道的职责划分：

- **`SubmitResult`** 回答"**你这一单**怎么了"——对调用者负责，一对一；
- **`Event`** 回答"**市场**发生了什么变化"——对世界负责，一对多。

一笔全额成交对世界的变化 = 若干笔成交，这些已经作为 `Trade` 事件报完了；它没有簿变化（订单从未挂过簿）、没有拒绝。什么都没有发生，就没有事件。反过来若规定"每笔单结束都补一个终态事件"，观察者收到的信息是冗余的（它自己能从成交事件推导），还强迫引擎维护"订单生命周期状态机"的全部阶段。我们选**事实流**模型：**无变化，无事件**。

（对照领域模型：FIX 协议的 execution report 是"订单生命周期"模型的代表——Accepted / PartiallyFilled / Filled / Canceled 逐个阶段上报。那是为"一方需要精确跟踪自己订单状态"设计的；我们这里那条需求由 `SubmitResult` 承担。两个模型都能用，我们的划分是：**状态流给调用者，事实流给世界。**）

### 1.3 用一个个字段回答"消费者需要知道什么"

现在给 `Event` 设计字段。方法是把消费者具象化——假设要写一个**行情镜像模块**（维护一份和引擎一致的簿）和一个**风控模块**（监控账户行为、拒绝原因），看它们在每种事实里各需要什么：

- 行情镜像收到 Trade：要**成交价、成交量、方向**，还要**双方订单 id**（行情明细/回放到订单级）；
- 行情镜像收到 OrderRested：要**订单 id、价格、量、方向**才能把这笔挂进自己的镜像；
- 行情镜像收到 OrderCanceled：要**订单 id**（从镜像里删掉）与**取消量**（对账）；
- 风控收到一切：还要知道**账户**（否则 STP 类规则没法写——这也预告了 §2 为什么要给订单加账户）；
- 风控收到 OrderRejected：要**原因码**。

把这些需求求并集，再减去"引擎里根本没有的信息"，得到字段：

```cpp
struct Event {
    EventKind            kind;                       // 这条事实是四类里的哪一类
    maelstrom::OrderId   order_id{0};                // 事实的主体：哪笔订单
    maelstrom::AccountId account{0};                 // 主体属于哪个账户
    maelstrom::Side      side = maelstrom::Side::Sell; // 主体方向
    maelstrom::OrderId   counterparty_id{0};         // 仅 Trade：对手方订单 id
    maelstrom::Price     price{0};                   // Trade=成交价（被动价）/ Rested=挂单价
    maelstrom::Quantity  quantity{0};                // Trade=成交量 / Rested=挂簿量 / Canceled=取消量
    RejectReason         reason = RejectReason::None; // 仅 OrderRejected
};
```

每种事实的字段含义（`{0}` 表示该字段在这种事实里没有意义，保持默认值）：

| 字段 | Trade | OrderRested | OrderCanceled | OrderRejected |
|---|---|---|---|---|
| `order_id` | 主动单 id | 挂簿的单 id | 被取消的单 id | 被拒的单 id |
| `account` | 主动方账户 | 该单账户 | 该单账户 | 该单账户 |
| `side` | 主动方向 | 该单方向 | 该单方向 | 该单方向 |
| `counterparty_id` | 被动单 id | `{0}` | `{0}` | `{0}` |
| `price` | 成交价（被动方价格） | 挂单价 | `{0}` | `{0}` |
| `quantity` | 成交量 | 挂簿量 | 取消量 | `{0}` |
| `reason` | None | None | None | 拒绝原因 |

两个设计决定值得单独说：

**为什么是一个结构体加 tag，不是 `std::variant<TradeEvent, RestedEvent, ...>`？** 这是真实的取舍，两种都对：

- `variant` 的优点：类型安全——读不到"不属于该种类"的字段，编译期杜绝了"给 Rejected 读 price"这类错误；缺点是每个消费者都要写 `std::visit`，写日志/序列化要按类型分别处理，调试时也不便整体打印；
- 固定结构体（本项目的选择）的优点：一条事件就是一个可整体拷贝/打印/memcmp 的值，订阅端一个 `switch (e.kind)` 走完；代价是"无效字段"占位置（每种事实浪费几十字节的 `{0}`），且语义靠契约与测试钉住而非编译器。

W4 的目标是"多订阅者 + 顺序 + 正确性"，观测数据量还远没到需要抠字节的时候；`variant` 版本列入扩展清单（文末「迁移练习」会让你评估它要改多少东西）。

**每个字段必须有默认值——这是类型契约，不是风格。** `Event ev{};` 之后每个字段都必须处于确定状态。上一周你已经为这条纪律付出过代价：你的 `SubmitResult` 少了默认成员初始化，差分 oracle 从你的类型里读出了未初始化的 `reason`——语义单测全绿，**只有差分测试能看见"类型对外契约"层面的问题**。W4 的 `EventDefaultsAreZeroed` 用例做的就是同一件事：钉死"`Event ev{}` 后六个数值字段全 0、reason 为 None"。

### 1.4 订阅的身份、退订与顺序

补齐 §1.1 的 A2/A3 两条缺口。

**订阅需要身份（`SubscriptionId`），而不是下标。** 理由在 B1 硬伤 1 已给出（erase 让下标错位）。给每个订阅发一个号：

```cpp
using SubscriptionId = std::uint64_t;
SubscriptionId subscribe(Handler handler);  // 返回订阅号，从 1 开始
bool unsubscribe(SubscriptionId id);        // 精确退订；不存在返回 false
```

**订阅号从 1 单调递增、永不复用。** 为什么不复用？一个具体的事故场景：风控模块持有 id=2，打算延迟退订（比如"处理完当前批次再退"）；期间它先退订了 2，另一个新订阅者拿到空闲的 2；风控的延迟退订到达，把**新订阅者**杀了。"下标复用"和"id 复用"是同一类 bug（悬垂句柄指向了别人）。单调递增让一个 id 在任何时刻只可能指向"当初拿到它的那个人"，退订语义从此无歧义。代价只是 `uint64_t` 永远涨——每秒订阅 10 亿次也要 500 年才回绕，可以忽略。

**投递顺序 = 订阅注册顺序，同步送达。** 两个决定各自的理由：

- **顺序**：事件流的一部分消费者是"记录流"（日志、回测），它们需要确定且可重现的顺序。按注册顺序遍历是最便宜、最容易讲清楚的默认；需要更复杂顺序策略的场景这里不存在。
- **同步**：`publish` 在 `submit` / `cancel` 返回前把所有事件全部送达。这样调用者可以把"世界已经知道这件事了"当作返回后的既定事实（你的日志一定写在 `SubmitResult` 被检查之前）。代价是：**慢订阅者会拖慢撮合**。解耦它需要异步队列 + 并发，那正是 W5 的题目——本周先用最简正确的同步版本，把复杂度记账，别提前还。

**两条约定（测试不覆盖，但请遵守）**：

- 分发期间不要 subscribe/unsubscribe（遍历 `vector` 时增删 = 迭代器失效）。我们不做防御（不复制副本、不加锁），单线程契约下这是"重入未定义"，文档写清楚即可；
- 订阅者不要抛异常（会穿透 `submit`）。异常安全的分发策略是个正经话题，留到 W5 之后正式化。

**W3 的 `set_trade_handler` 保留，与总线并行。** 理由有二：旧代码（和 W3 的 151 个测试）依赖它，换掉就是无谓的破坏性变更；它的语义也确实更窄更快（单消费者、只见成交）。总线捕获的事实集是它的超集——`BusAndTradeHandlerAreIndependent` 用例把"两条通道互不干扰"钉死。

### 1.5 引擎在哪些时刻广播（发布点推导）

有了消息与总线，还剩最后一个问题：**引擎的哪些代码行应该 `publish`？** 方法是把 W3 `submit` 的每个出口再走一遍，逐行问"这一行发生的事，世界需要知道吗"：

| `submit`/`cancel` 的哪一步 | 可观察变化 | 发什么事件 |
|---|---|---|
| 五个校验分支任一命中 | 拒绝本身是事实（无市场变化） | `OrderRejected`（带 reason） |
| 循环中吃掉对手盘队头 | 发生一笔成交 | `Trade`（紧随 `consume_best` 之后发，顺序即撮合顺序） |
| STP-CancelOldest 撤掉一笔自己的挂单 | 簿少了一笔 | `OrderCanceled`（被撤的单，§2 详解） |
| STP-CancelNewest 砍掉主动单余量 | 主动单死亡 | `OrderCanceled`（主动单，§2 详解） |
| 余量挂入簿中 | 簿多了一笔 | `OrderRested` |
| Market/IOC 余量被丢弃 | 主动单死亡 | `OrderCanceled` |
| `cancel(id)` 成功 | 簿少了一笔 | `OrderCanceled` |

七处发布点，一处不能少、一处不能多。漏一个的典型后果：观察者收到一串 `Trade` 之后什么都没有，它无法判断"余量挂簿了"还是"余量被丢掉了"——Market/IOC 那行（表里最容易漏的一处）就是把这种"静默死亡"变成事件的。多一个的典型后果：STP-CancelNewest 的取消在循环里已经发过，余量段如果按 Market/IOC 的逻辑再发一次，事件数就多了一（后面任务 4 会提醒你怎么防）。

注意表里**没有"全额成交"这一行**——这正是 §1.2 "事实流"模型的推论：交易已报、无簿变化，就没有终态事件。

**事件顺序的完整口径**：一个 `submit` 的事件序列 = **实际发生顺序**（成交与 CancelOldest 的撤销按撮合循环的时间顺序交错）＋**终态事件（若有）排在最后**；被拒的 submit 恰好一个 `OrderRejected`；`cancel` 成功恰好一个 `OrderCanceled`。

### 1.6 需求 A 的正式契约

```cpp
// 多订阅者事件总线：同步分发，按订阅注册顺序逐一回调。
class EventBus {
public:
    using Handler = std::function<void(const Event &)>;

    SubscriptionId subscribe(Handler handler); // 从 1 递增，不复用
    bool unsubscribe(SubscriptionId id);       // 不存在返回 false（含二次退订）
    void publish(const Event &event);          // 按注册顺序同步分发
    [[nodiscard]] std::size_t subscriber_count() const;
};
```

- `EventBus` 是头文件内联的小类（不需要 .cpp）：`vector<槽位> + 自增 id` 就够了。
- `MatchingEngine` 持有一个 `EventBus bus_;` 成员并对外转发：

```cpp
[[nodiscard]] SubscriptionId subscribe(EventBus::Handler handler);
bool unsubscribe(SubscriptionId id);
```

- `Event` / `EventBus` / `EventKind` / `SubscriptionId` 全部放在 `matching_engine.h`（**不新建头文件**）：`Event` 需要 `RejectReason`（W3 就在这个头里），拆出去会形成循环包含。真想拆，先考虑把 `RejectReason` 抽到独立头——留作思考题。
- 字段语义与默认值以 §1.3 的表为准；发布点以 §1.5 的表为准；订阅号从 1 开始、单调、不复用。

---

## 2. 需求 B 的推导：自成交防护

### 2.1 判定放在哪里：为什么不是"下单前扫描"

先写一个自然的基线：**在 `submit` 入口处扫描对手侧，发现"自己的挂单且价位与我的限价交叉"就处理**。它看起来覆盖得挺全，但有一个具体的过度拦截反例：

```
簿上：别人的卖单 100 × 10（最优），你自己的卖单 102 × 4
新单：买 5，限价 102
入口扫描：对手侧有自己的挂单 102，与限价 102 交叉 → 判定为自成交冲突，拦下
实际执行：100 档的 10 手完全够吃这 5 手，单子全额成交，根本碰不到 102 —— 不该拦
```

一句话：**"价位在我的限价范围内"不等于"我会碰到它"**。撮合逐档推进，深档位的"自己人"在前面的档位被吃完之前永远不会被触及。入口扫描的毛病有两条：**语义上过度拦截**（把永远不会碰到的自己人算成冲突），**成本上最坏 O(全簿)**（热路径上每次提交都扫一遍对手侧）。

同一原则的另一半由 `MatchingW4.StpTriggersOnlyAtContactNotWholeBook` 钉住：自己账户在 102 的挂单，限价 101 的新单吃到 100 档后余量正常挂在 101，自己的那笔挂单动都不动。

第二个候选是"成交后再冲销/撤销"。直接排除：成交是**不可回滚**的——事件已经广播、被动单可能已经被吃光、对方账户的成交是真的。防御必须在交易发生**之前**。

正确的判定点于是唯一地确定下来：**撮合循环内，交叉判定通过之后、`consume_best` 之前**——也就是"这笔成交即将发生"的瞬间，只看**队头这一笔**。

为什么只看队头就够？因为撮合循环每次只吃队头（W3 的 `consume_best` 语义），冲突也只可能在队头发生；更深的挂单还没轮到被碰。这个位置每次判定是 O(1)，语义上恰好是"执行接触点判定"（judge at the point of execution）——真实交易所的 STP 普遍也是这个口径。

### 2.2 用什么识别"自己"：账户、0 号哨兵与向后兼容

"自己人"需要身份。`Order` 上目前没有主体信息，所以需要加一个账户字段。三个决定：

**决定 1：`Order` 追加 `AccountId account{0}`（新增强类型，放在 `type` 之后）。** 沿用 W3 加 `type` 时的全套纪律：追加在最后、带默认值、不给聚合类型加构造函数——这样 W1/W2 的四段初始化和 W3 的五段初始化原样编译通过。`AccountId` 照 `OrderId` 的模板做（构造 + `get()` + 比较）：账户和订单都是一段 `int64`，强类型让"把账户当订单传"这类错误在编译期消失，这是 W1 就立下的规矩，继续执行。

**决定 2：`account == 0` 表示"没有账户信息"，永不触发 STP。** 触发条件因此是 `o.account != 0 && best->account == o.account`。

为什么要这个豁免？两个层面的理由，一个工程一个语义：

- **工程（向后兼容）**：W3 的全部订单都没有账户（默认 0）。若 0 也参与 STP，任何两笔 W3 订单都会被当成"同一主体"而互相拦截——W3 的 151 个测试与 40 个差分种子会当场全线变红。0 豁免把"没提供账户信息"的旧订单语义原样保留。
- **语义**：0 是**保留的"空账户"**——账户 id 由系统分配（本项目和真实 OMS 一样，不会给一个真实主体分配 0）。"没有提供信息"不能推断出"同一主体"，所以不触发。代价也如实写进契约：**匿名订单得不到防护**，调用方想要 STP 就必须带账户。

**决定 3：非 0 即可触发——负数账户也算。** 判据是 `!= 0` 而不是 `> 0`（`MatchingW4.NegativeAccountIdStillTriggersStp` 钉住：account=-5 的买单撞上 account=-5 的挂单照样触发）。账户 id 的值域是"除 0 外的任何 int64"。

**一个值得停留的对比**：W3 你刚学过"别用哨兵值表达语义"——Market 单的 `price` 不参与判定，我们拒绝"`price == 0` 当市价"这种写法，坚持由 `OrderType` 承担语义。为什么这里却接受"0 号哨兵"？因为两者性质不同：

- Market 的"无限价"是一种**语义模式**（影响校验、撮合、FOK 等一整串行为），它应该由类型的维度（`OrderType`）表达；
- 账户的 0 是**同一个维度上的一个保留取值**（"是不是某个主体"，0 = 无主体），用值表达是自然的，前提是把约定写进契约并用测试钉死。

替代方案是 `std::optional<AccountId> account;`——类型层面显式表达"可能没有账户"，编译器帮你防混淆。它完全可行（聚合初始化也兼容），取舍在于：`Order` 会离"简单的字段集合"远一步、热点路径多一层分支语义、而现有测试的契约已把"0 = 无账户"钉死。这里选哨兵的理由主要是**兼容与简单**；如果你在别的项目里更想要 `optional`，那也是一个有理由的选择——把选择依据（值域约定 vs 类型显式性）想清楚，比选哪个更重要。

### 2.3 碰撞后谁让路：策略空间与本周的两个选择

触发条件一旦成立，"自己撞自己"必须由某一方让路。真实交易所的 STP 策略空间大致五种，先看全貌再收窄：

| 策略 | 让路的是谁 | 说明 |
|---|---|---|
| **CancelNewest** | 撞上来的新单 | 新单余量被取消，老挂单不动 |
| **CancelOldest** | 已有的老挂单 | 老挂单被撤，新单继续找对手 |
| CancelBoth | 双方 | 新单余量 + 老挂单一起取消 |
| Decrement | 双方各让一部分 | 把两边减量到"刚好不交叉"再收手 |
| RejectIncoming | 新单整单 | 直接拒绝，连"已成交的部分"都要求不发生 |

本周实现**前两个**，理由是它们恰好覆盖两个方向的所有权决策：一个改**在途订单**（CancelNewest：杀主动单），一个改**簿上订单**（CancelOldest：杀被动单）。把两套代码路径都亲手写一遍，"策略差异应该收敛到哪里"这个问题你才有实感。其余三种、以及"按账户对配置不同策略"列入扩展清单。

两个策略的语义，逐条推到清楚：

**CancelNewest（默认策略）——杀主动单余量：**

- 已经成交的部分**保留**。那些成交的对手是**别的账户**，是真实发生、已经广播的市场事件——不可能"收回"。STP 只解决剩余的冲突（`StpCancelNewestKeepsEarlierTrades`：先吃 acct1 的 5 手，再撞上 acct7 自己的挂单，余量 5 被砍，成交序保留 1 笔）。
- 被撞的**挂单纹丝不动**。它是先来的、无辜的（`StpCancelNewestBlocksAndCancelsRemainder`：撞上自己的挂单后，那笔挂单原样还在簿上）。
- 余量**不挂簿**。为什么不能挂？见 §2.4。
- 发一个 `OrderCanceled{主动单 id, quantity=被取消的余量}`。
- outcome：`resting == 0 且 filled < quantity` → 落进 W3 既有的 **`Cancelled`** 分类（这句话很重要，§2.4 展开）。

**CancelOldest——杀被动挂单：**

- 撤掉队头那笔**自己的挂单**（整单；等价于对它做一次 `cancel`：摘链、删空档、释放，发行情里"簿少了一笔"的 `OrderCanceled{被撤单 id, quantity=其剩余量}`）。
- 主动单**不消耗任何量**（没有成交发生），继续循环找下一个对手：同价下一笔如果又是自己的，再次触发，**依次后撤**——`StpCancelOldestChainsThroughMultipleOwn` 就是这个链式场景（撤 id1、撤 id2、成交 id3、余量挂簿，事件流以此为序）。
- 之后主动单可能成交、可能余量挂簿（`StpAgainstOwnRestingRemainderCancelOldest`：撤掉自己的买一后，卖单余量挂上了卖盘）。

### 2.4 两个必须想清楚的后果

**后果 1：CancelNewest 的余量绝不能挂簿——否则簿当场交叉。**

想一个具体反例：买 10@100 撞上自己卖 5@100。此刻对手侧最优价 100 与买价 100 交叉。如果按"普通限价单余量挂簿"的逻辑，把这 10 手挂回买盘 100——而自己那笔卖单还挂在 100 档——簿上立刻变成 `best_bid(100) >= best_ask(100)`，**交叉簿**。W2/W3 维持了一整周的"簿上不存在可成交的两笔单"不变量，被这一行代码打破；而且状态自相矛盾：你刚刚才判定"这笔单不该与那笔单成交"，却把两笔都留在了簿上等着别人来看。

所以 CN 的余量必须**消失**（撤销即死）。这也是为什么实现里需要一个 `self_killed` 标记：余量处理段（W3 里那句 `can_rest = (type == Limit || PostOnly)`）必须知道"这笔单的余量是被 STP 砍掉的，不许挂"。没有这个标记会发生什么，差分测试会在第一个种子就抓住：`resting` 与簿状态全错，而且簿可能交叉。

**后果 2：CancelOldest 的循环必然终止。**

CO 的每次循环迭代只有三种可能：撤掉一笔挂单（簿严格缩小）、成交一笔（remaining 严格缩小）、或 break。两者都是有界量，所以有限步内终止，不会死循环。顺手确认另一件事：撤的只可能是**对手侧**的挂单（触发条件是"队头与我对撞"），所以永远不会撤掉主动单自己。

**顺带一个漂亮的性质：outcome 不需要任何新规则。** 看 CN 的结局——`resting == 0`、`filled < quantity`——W3 的 outcome 规则表自动把它分类为 `Cancelled`；CO 的结局可能落在 `Filled / PartiallyFilled / Rested / Cancelled` 里的任何一个，也全部由既有规则决定。你的工作只是**正确维护状态**（余量去哪、簿怎么变），结果分类自己就对了。这是契约扩展时值得体会的信号：**如果需要给 outcome 加新值，往往说明状态维护出了偏差**。

### 2.5 FOK 与预检：全额成交的承诺不可被 STP 打破

本节是 W4 里**最重要的一处设计修正**，值得慢慢读。

初版契约让 FOK 的预检对账户失明：`matchable_quantity` 把"自己人的挂单"也计入可成交量，执行时撞上自己再交给 STP 处理。这会产出一个**违反 FOK 语义**的结果，具体走一遍：

```
簿上：acct1 卖 3@100（前）、acct7 卖 5@100（后）
acct7 提交 FOK 买 8 @101
初版预检：matchable_quantity = 8 ≥ 8 -> 放行
初版执行：吃掉 acct1 的 3 手，撞上自己的 5 手 -> CN 砍掉余量
结局：部分成交 3 手后被取消 —— 一个"部分成交的 FOK"
```

FOK 的定义就是"无法立即全部成交则不成交"（全成或全不成）——部分成交的 FOK 在真实市场上不存在。这不是"为了少扫一次簿的简化"，而是**把违约行为写进了契约**：先承诺"要么全成"，再在执行阶段自己把承诺打折。同类漏洞在 `CancelOldest` 下也存在：初版预检计数包含了自己的挂单，而自己的挂单在执行时只会被**撤掉**、从不提供成交——"预检通过、执行时流动性耗尽"同样产出部分成交的 FOK。

修正后的原则三句话（本版契约）：

1. **FOK 的全额成交约束不可放宽**——订单语义的正确性优先于任何实现上的便利；
2. **STP 决定"可达流动性"**——预检必须按"当前策略下真正能吃到多少"判定，而不是"簿上有多少"；
3. **失败的 FOK 不修改订单簿**——预检是纯读操作：不成交、不撤 Maker、簿不变。

**可达流动性（reachable liquidity）的定义，逐策略**：

| 策略 | 可达流动性 | 为什么 |
|---|---|---|
| `CancelNewest` | 按价格时间优先遍历限价内挂单，累计**他人**量，**遇到第一笔自己的挂单即停止** | 一撞上自己，余量就会被砍——后面的挂单永远碰不到 |
| `CancelOldest` | 限价内**全部他人**量（遇到自己的挂单跳过、继续累计；凑够即停） | 自己的挂单会被撤掉，既不提供成交也不阻塞 |

预检通过（可达量 ≥ 需求量）才进入撮合循环；否则 `Rejected{FokNotFillable}`，零副作用（一条 `OrderRejected` 事件之外什么都没有）。

**一个例子走两遍**（测试原样钉住的场景）：acct7 提交 FOK 买 10@100，卖盘按价格时间优先为 M1(acct1, 99, 4)、M2(acct7, 99, 3)、M3(acct2, 100, 6)：

| 策略 | 预检 | 结果 |
|---|---|---|
| `CancelNewest` | 累计 M1 得 4，撞上 M2（自己的）即停：4 < 10 | **整体拒绝**；三笔 Maker 原样，簿不变 |
| `CancelOldest` | 跳过 M2，累计 4 + 6 = 10 ≥ 10 | **全额成交**：M1 成交 4 → M2 被撤（发取消事件）→ M3 成交 6 |

把 M3 改成 5 手，看两个必须想清楚的边界：

- `CancelOldest`：4 + 5 = 9 < 10 → **整体拒绝**，且 M2 **不得被撤**。预检是只读的——"先成交 M1、再撤 M2、最后发现不够"是不允许的，那会留下一个被无故撤销的 Maker。这就是"失败的 FOK 不修改订单簿"的含义。
- **如果 M1 本身就有 10 手**，FOK 在接触 M2 之前已全额成交——两种策略下 M2 都不该被碰。STP 与预检都必须遵守**实际撮合顺序**，而不是"扫描范围内有自己人就插手"（`FokFillsBeforeContactingOwnBothPolicies` 钉住）。

**POST-ONLY 为什么继续对账户失明？** 这不是遗漏，而是两个预检的"安全方向"不同：

- POST-ONLY 的承诺是"绝不主动吃单"。**保守地多拒一笔永远不违反这个承诺**——拒单是安全方向，扯不上 STP；
- FOK 的承诺是"全成或全不成"。**过度乐观地接受会直接违约**——部分成交是危险方向，必须在成交发生之前拦下。

一句话记住：**预检的职责是保证"通过预检 ⇒ 执行必然履约"。** 履约承诺可能被 STP 打破的（FOK），预检必须懂 STP；承诺不可能被打破的（POST-ONLY），多一分保守都是安全的。

**实现形态**：不需要为账户建索引。簿提供一次**只读的、按价格时间优先的遍历原语**（任务 3 会设计它；参考实现叫 `for_each_maker`），引擎在其中累计可达流动性、凑够即停。最坏复杂度 O(限价内挂单数)，只有 FOK 走这条路；还可以先用 `matchable_quantity`（含自己人的总量，是可达量的**上界**）做第一道快速排除——总量都不够直接拒绝，连逐笔扫描都省了。

**预检与执行的等价性依赖什么？** 依赖"预检之后、执行之前，簿不再被改动"。W4 的单写者假设（单线程 + 回调不得重入改簿，§1.4）保证这一点——回调纪律在这里第二次发挥作用。将来进入多线程或允许重入回调时，这个不变量需要重新论证（W5 的题目之一）。

### 2.6 需求 B 的正式契约

- **触发条件**：撮合循环内、交叉判定通过后、吃单前，**只看队头这一笔**：
  `o.account != 0 && best->account == o.account`（非 0 即触发，负数也算）。
- **CancelNewest（默认）**：主动单余量全部取消（已成交部分保留）；队头挂单不动；余量不挂簿；
  发 `OrderCanceled{主动单, quantity=余量}`；outcome 由既有规则自然落 `Cancelled`。
- **CancelOldest**：撤销队头挂单（整单，发 `OrderCanceled{该单, quantity=其剩余量}`）；撮合继续，
  可连续撤销多笔；主动单不消耗量。
- **策略切换**：`set_self_match_policy(policy)` 随时可调，只影响其后的 `submit`；默认 `CancelNewest`。
- **FOK 预检按可达流动性判定**（§2.5）：CN 下累计到第一笔自己的挂单为止、CO 下只数他人；
  通过 ⇔ 必然全额成交；未通过 ⇒ `Rejected{FokNotFillable}` 且**零副作用**。
  **FOK 永不部分成交、永不挂簿**——余量挂簿只属于 Limit/PostOnly。
- **POST-ONLY 预检保持纯价格口径（账户盲）**；**账户不参与校验**（除 0 外任何 int64 都是合法账户）。
- **余量挂簿携带账户**（`Order r = o;` 自然带上）：此后别人撞上这本余量，按它的账户判 STP。
- **改价保留身份**：`OrderBook::modify(id, price, qty)` 重挂时必须原样保留该单的 `account` 与
  `type`——若用 `Order{price, id, qty, side}` 重新构造，账户会退回 0，**这张单从此不再受 STP 保护**。

对应到类型：

```cpp
enum class SelfMatchPolicy : std::uint8_t {
    CancelNewest, // 撞上自己的挂单：主动单余量取消，挂单不动
    CancelOldest, // 撞上自己的挂单：撤掉该挂单，撮合继续
};
```

---

## 3. 验收方式的变化：事件流为什么也要进差分

W4 的验收为什么必须扩展而不是重复 W3 的 92 个用例？因为这次同时动了两样东西：

1. **状态空间炸了**：账户（0/1/2）× 五种订单类型 × 两种策略 × 事件序列，手写用例只能枚举"想得到的"组合——"FOK 的可达流动性在两种策略下的分歧""IOC 连续撞三笔自己的挂单""策略在会话中途切换"这类交叉场景，靠手写是覆盖不完的。
2. **输出多了一条通道**：`SubmitResult` 是**汇总结果**（一个数量守恒的摘要），而**事件流是顺序敏感的完整契约**——第 k 个事件的每个字段、它的先后位置，都在契约里。下一段的教训说明了"顺序敏感 + 逐字节"的契约面为什么必须由差分来守。

所以 W4 的差分机制这样升级：

- **oracle 同步升级**：参考引擎（`naive_matching_engine_w4.h`）在慢速直白风格上增加账户、STP 两策略、事件记录；FOK 预检在挂单集合的**副本上干跑一次撮合**（纯读、零副作用），与引擎的"可达流动性扫描"是同一语义的两种写法；每一步产出三元组 **(SubmitResult, 事件流, 簿状态)**，与你的引擎逐项比对。
- **40 个种子分工**：奇数种子跑 CancelNewest、偶数种子跑 CancelOldest，保证两种策略都被大量随机场景扫过；随机注入非法价/量（沿用 W3）。FOK × 账户 × 策略的随机组合会在这里被打穿——**预检与执行只要有一丝不一致，差分就会把它变成红色的 step**。
- **失败信息自带现场**：`seed / step / policy / type / side / acct`，事件不一致时还给第几个事件与双方的 `kind`。

**上一周的教训回放**：W3 差分测试曾经是你唯一抓住"`SubmitResult.reason` 未初始化"的那层验收——语义单测看不见"类型对外契约"。今天事件流把契约面扩大了一个量级（每条事件 8 个字段 × 顺序），差分的价值同步放大：**它不关心你的引擎"看起来对不对"，它把你引擎对外吐出的每一个字节都当作契约来核对。**

---

## 任务 1 ｜ 给订单装上账户

**① 目标（可验收）**

`Order` 以追加方式获得 `AccountId account{0}`；`cmake --build` 通过；`ctest` 的 151 个旧测试**一行不改**仍全绿；后续 STP 用例能表达"同一主体"。

**② 为什么现在做不到**

STP 的触发条件需要判定"这两笔单是不是同一个主体"（§2.2），而 `Order` 没有位置表达主体。同时 `Order` 是聚合类型，直接插字段或加构造函数会连累 W1/W2/W3 的全部旧代码——这是你在 W3 加 `type` 时处理过的同一类课题，再练一遍。

**③ 提示**

1. `types.h` 里照 `OrderId` 的模板加 `AccountId`（构造、`get()`、`==`/`!=`）。强类型的意义：账户和订单在 ABI 上都是一段 int64，在类型系统里是两个世界。
2. `order.h` 里把 `AccountId account{0};` 加在**最后一行**——三条纪律：放最后（聚合初始化按声明顺序填，旧写法省略它）、带默认值（`{0}` 的语义见 §2.2：无账户，永不触发 STP）、**不加构造函数**（保持聚合类型）。
3. 注意新增后的字段顺序是 `{Price, OrderId, Quantity, Side, OrderType, AccountId}`——W4 测试的 `mk()` 按这个顺序构造六段初始化。

**④ 验收**

```bash
cmake --build build -j
ctest --test-dir build          # 151 个旧测试全绿 —— 你没破坏任何东西
```

**⑤ 常见错误**

| 症状 | 病因 |
|---|---|
| 旧测试编译失败 | 字段插在了 `type` 之前；或给 `Order` 加了构造函数（聚合性丢失） |
| 行为诡异 | 漏写 `{0}`（账户处于不确定值，可能随机触发 STP） |
| 想当然的"改进" | 把 `Order` 改成封装类——W1–W3 的一切依赖聚合初始化的代码全炸 |

**⑥ 答案**：附录 A1。

---

## 任务 2 ｜ 事件类型与总线（类型设计先行）

**① 目标（可验收）**

`SubscriptionId` / `EventKind` / `Event` / `EventBus` / `SelfMatchPolicy` 全部就位且与 §1.3、§1.6、§2.6 的契约一致；`Event` 的六个数值字段与 `reason` 都有默认成员初始化（`EventDefaultsAreZeroed` 会钉）；`MatchingEngine` 增加三个转发方法（`subscribe` / `unsubscribe` / `set_self_match_policy`）与成员 `bus_`、`policy_ = CancelNewest`。库编译通过，旧的 151 个测试仍全绿（只加类型，没动行为）。

**② 为什么先做类型**

事件系统最容易写成"随手往回调里塞参数"，返工成本高。§1.2 已经推导出消息种类、§1.3 推导出字段、§1.4 推导出总线语义——现在把这些推导**落成类型**，再动引擎。类型没定对，发布点写多少返工多少。

**③ 提示**

1. 把 §1.3 的字段表直接翻译成结构体，**字段顺序照抄**（差分测试逐字段比对，顺序错了是编译错误；名字对了但语义错了是差分红）。
2. `EventBus` 是头文件内联的小类：`vector<Slot>`（`Slot = {id, handler}`）+ `next_id_ = 1`。`subscribe` 尾插并返回自增前的 id；`unsubscribe` 线性找 id、找到则 erase 并返回 true；`publish` range-for 顺序调用。都不复杂——把"为什么这么简单"讲得出来（§1.4）比写出来更重要。
3. `Event` 放 `matching_engine.h`，不新建文件（§1.6 讲了循环包含的原因）。
4. `MatchingEngine` 里 `policy_` 的默认值是 `CancelNewest`——默认策略的选择写进契约（§2.6）。

**④ 验收**

```bash
cmake --build build -j
ctest --test-dir build    # 151 仍全绿；W4 测试现在红是预期的（引擎还没发事件、没做 STP）
```

**⑤ 常见错误**

| 症状 | 病因 |
|---|---|
| 差分全红在"字段不一致" | `Event` 字段顺序/类型与契约不符（如 `account` 用了 `int64_t` 而不是 `AccountId`） |
| `EventDefaultsAreZeroed` 红 | 漏了某个默认成员初始化 |
| `SubscriptionIdsStartAtOneAndNeverReuse` 红 | 从 0 开始；或退订后把 id 回收复用 |
| `SubscriberCountTracks` 红 | `unsubscribe` 没真删/删错元素 |

**⑥ 答案**：附录 A2。

---

## 任务 3 ｜ 簿的只读遍历原语 + STP 进撮合循环 + FOK 可达流动性预检

**① 目标（可验收）**

1. `MatchingW4.Stp*`（9 个）+ 同族的 4 个边界用例（`ZeroAccountNeverTriggersStp` / `DifferentAccountsTradeNormally` / `PolicySwitchMidStream` / `NegativeAccountIdStillTriggersStp`）+ `MatchingW4.Fok*`（8 个）全绿；碰撞路径上的簿状态、事件、outcome 逐条符合 §2.6。
2. `BookPrimitivesW4.ModifyKeepsAccountAndType` 全绿（改价保留账户与类型）。
3. W1–W3 的 151 个测试保持全绿（改动最大的一个任务，每做一步就回归一次）。

**② 为什么这些用例现在全红**

引擎做两件事的方式都要改：撮合循环还不知道"自己人"存在（§2.1–§2.4：接触点判定、触发条件、两种策略、余量不挂簿与循环终止）；FOK 的预检还在按"簿上有多少"判定（§2.5：必须按"当前策略下真正能吃到多少"判定，否则 FOK 的承诺会被 STP 打破）。

**③ 提示**

按三个小步走，每步都能单独验证：

1. **簿的只读遍历原语**（`order_book.h` / `order_book.cpp`）：新增一个"按价格时间优先只读遍历限价内挂单"的公共方法。参考形态：

   ```cpp
   // visit 返回 false 立即停止遍历；方法自身不改簿、不分配。
   void for_each_maker(Side side, std::optional<Price> limit,
                       const std::function<bool(const Order &)> &visit) const;
   ```

   实现就是把 `matchable_quantity` 的双向 if/else 骨架换成"逐档、档内从 `head` 沿链表走"。**不要在簿里建账户索引**——一次只读遍历足够，而且不改任何数据结构。自测：先让 W3 的 `BookPrimitivesW3.*` 保持全绿（你没动旧原语），本方法暂时没有调用方也不影响构建。

2. **FOK 预检替换**（`matching_engine.cpp`）：把原来的"`matchable_quantity < 需求量` 就拒"换成 §2.5 的两层判定——3a 用 `matchable_quantity` 做快速排除（总量不足直接拒）；3b 用 `for_each_maker` 按策略累计可达流动性（CN 撞上自己即停；CO 跳过自己继续累计；凑够即停），不足则 `reject(FokNotFillable)`。注意整个预检**只读**——失败路径不得留下任何簿变化（那会挂掉 `FokCancelOldestRejectsWhenReachInsufficient`）。
   **如果 FOK 走的是"先进入撮合循环再说"的旧结构**，你会得到"部分成交的 FOK"——`MatchingW4.Fok*` 会集体红，`Cancelled + filled>0` 是它的特征症状。

3. **STP 进撮合循环**：
   - **单点插入**：循环内、`crosses` 通过之后、`consume_best` 之前。只判队头。
   - **CancelNewest**：发 `OrderCanceled{主动单, 余量}` → 置 `self_killed = true` → `break`。关键联动：余量段的 `can_rest` 必须排除 `self_killed`，否则被砍的余量挂回簿上（§2.4 的交叉簿反例）。outcome 不需要新代码——既有规则自动落 `Cancelled`。通过 FOK 预检的单在 CN 下**永远不会**走到这里（可达流动性的定义保证吃到自己的挂单之前已经填满）。
   - **CancelOldest**：发 `OrderCanceled{队头单, 其剩余量}` → `book_.cancel(队头 id)` → `continue`。注意不消耗 `remaining`，且"同价下一笔可能又是自己的"（链式撤销由循环自然处理）。通过 FOK 预检的单在 CO 下可能走到这里（撤退自己的 Maker 后继续填满）。
   - **POST-ONLY 预检保持不动**（价格口径，§2.5 讲了两者"安全方向"的不同）。
   - **改价保留身份**：检查你的 `OrderBook::modify(id, price, qty)`——若实现用 `Order{price, id, qty, side}` 重新构造节点，把 `account`/`type` 从旧订单上复制过来（或直接在原节点上改 price/quantity）。否则同一个单改价前后 STP 行为不同，`BookPrimitivesW4.ModifyKeepsAccountAndType` 会红。

4. 推进顺序建议：先让 `StpCancelNewest*` 一族绿（最简单）→ `StpCancelOldest*` → `Fok*` 八连 → `PolicySwitchMidStream` / `NegativeAccountId` 收尾。

**④ 验收**

```bash
cmake --build build -j
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.Stp*:MatchingW4.Fok*:BookPrimitivesW4.*'
ctest --test-dir build    # 151 个旧测试仍全绿
```

**⑤ 常见错误**

| 症状 | 病因 |
|---|---|
| FOK 出现 `Cancelled + filled>0` | 预检仍是"簿上有多少"口径（部分成交的 FOK = 违约），没换成可达流动性 |
| CO 下 FOK 预检通过但执行时差量 | 预检把自己账户的挂单也计入了可达量（CO 只数他人量） |
| 失败的 FOK 之后簿变了（自己的挂单被撤） | 预检动了簿——它必须是纯读；撤销只能发生在真实撮合里 |
| 自己的挂单在限价之外仍被撤/仍阻止 FOK | 遍历没按交叉范围过滤（闭区间：买 `price >= limit` / 卖 `price <= limit`） |
| `StpPostOnlyPrecheckBlindToAccount` 红 | 把 POST-ONLY 也改成了账户感知（它保持价格口径） |
| `ModifyKeepsAccountAndType` 红 | 改价重新构造了 Order，account/type 丢失 |
| CN 后 `resting` 不为 0 / 簿出现交叉 | 余量挂回簿了（`self_killed` 没接进 `can_rest`） |
| CO 卡死或行为错 | 忘了 `continue`（或错误地消耗了 `remaining`） |
| 负数账户不触发 | 用了 `> 0` 而不是 `!= 0` |
| CO 少发事件 | 撤挂单忘了发 `OrderCanceled` |

**⑥ 答案**：附录 A3。

---

## 任务 4 ｜ 引擎事件发布点

**① 目标（可验收）**

`MatchingW4.EventSequence*` / `MatchingW4.CancelEmits*` / `MatchingW4.BusAndTradeHandler*` 全绿；任一 `submit` / `cancel` 的事件流与 §1.5 的表逐条一致（条数、顺序、字段）。

**② 为什么还不绿**

任务 3 只改了循环中部；事件是**全程**输出——拒绝、成交、挂簿、Market/IOC 取消、STP 两种取消、外部撤单，共七处出口，一处不能漏、一处不能重（§1.5 推导）。

**③ 提示**

1. 数着 §1.5 的表写：**7 处**。最容易漏的是 Market/IOC 余量取消（原因：W3 里它只是个 outcome 分类，没有任何显式动作，容易被当成"没有事发生"）。
2. **拒绝路径收口**：五个校验分支各写一遍发布是 bug 温床，用一个小 lambda（发 `OrderRejected` 并构造结果）让五个 `return` 全走它。
3. **`cancel()` 的顺序**：先 `book_.find(id)` 捕获账户/方向/量 → `book_.cancel(id)` → 发布。顺序反了就是拿着已释放的数据发事件（ASan 会教你）。
4. **CN 的去重**：CancelNewest 的取消事件已在循环里发过，余量段的 Market/IOC 取消分支要跳过（`!self_killed`）；否则事件数多一，差分第一个种子就红。
5. **不要顺手补对称**：全额成交**不发**终态事件（§1.2 的事实流模型），别因为"看起来不整齐"给它补一个。
6. W3 的 `handler_(t)` 原样保留（§1.4 末尾），与总线并行。

**④ 验收**

```bash
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.EventSequence*:MatchingW4.CancelEmits*:MatchingW4.BusAndTradeHandler*'
```

**⑤ 常见错误**

| 症状 | 病因 |
|---|---|
| 差分"事件数不一致" | 漏发（最常见：Market/IOC 取消）或重复发（CN 取消） |
| 差分"第 k 个事件不一致" | 字段填错（Rested 的 price/account 漏填）或顺序错（CO 的撤销没插在成交位置） |
| ASan use-after-free | `cancel()` 先删后读 |
| `BusAndTradeHandlerAreIndependent` 红 | 把某条通道整个换掉了，而不是并行 |

**⑥ 答案**：附录 A3（与任务 3 同一段参考实现）。

---

## 任务 5 ｜ 把裁判挂进构建

**① 目标**

裁判测试源被构建系统认识；一次全量构建后 W4 用例可被执行。

**② 为什么需要**

`tests/` 里已经躺着 84 个 W4 用例，但 `add_executable` 的源列表不认识新文件——不改 CMake，你写再多也没人告诉你对没对。

**③ 提示**

1. `tests/CMakeLists.txt` 的 `add_executable` 源列表加一行 `test_matching_engine_w4.cpp`（**只改这一行**，和 W3 时一样）。`naive_matching_engine_w4.h` 是头文件，被测试源 `#include`，不需要登记。
2. 库目标**不用改**：W4 没有新的 .cpp，也没有新的头文件（§1.6）。
3. 改了 CMake 必须重新配置再构建：

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DMAELSTROM_BUILD_TESTS=ON -DMAELSTROM_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

4. 认一下过滤尺子：

```bash
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.*'             # 语义 + 生命周期（35）
./build/tests/maelstrom_tests --gtest_filter='EventBusW4.*'             # 总线（8）
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.Stp*'          # STP 主力一族（9）
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.Fok*'          # FOK × STP 可达流动性（8）
./build/tests/maelstrom_tests --gtest_filter='BookPrimitivesW4.*'       # 簿原语：改价保留账户（1）
./build/tests/maelstrom_tests --gtest_filter='Seeds/MatchingDifferentialW4.RandomSubmitSequence/7'
ctest --test-dir build --output-on-failure                              # 全量（含 W1-W3，共 235）
```

**④ 验收**：上述命令能列出/运行对应用例（语义用例此时红着——预期内的红，是起跑线）。

**⑤ 答案**：附录 A4（一处改动，一行）。

---

## 任务 6 ｜ 语义 + 总线全绿 + 口试自检

**① 目标**

```bash
./build/tests/maelstrom_tests --gtest_filter='MatchingW4.*:EventBusW4.*:BookPrimitivesW4.*'   # 35 + 8 + 1 = 44
[  PASSED  ]  44 tests
```

并且下面每个问题你能对着代码讲出来（写进笔记）。

**② 提示**

1. 失败按族缩小范围：`Stp*` / `Fok*` / `EventSequence*` / `Huge*` / `Lifetime*`；差分失败信息带 seed/step/policy/事件序号，抄下来可以二分定位。
2. **口试自检**：
   - `account == 0` 为什么永不触发 STP？把这个豁免删掉，W3 的哪些测试会红、为什么？
   - CancelNewest 的余量为什么必须消失而不是挂簿？（提示：对着那个"交叉簿"反例讲。）
   - CancelOldest 的循环为什么一定终止？它会不会撤掉主动单自己？
   - CN / CO 在"成交序列、事件序列、outcome"三个维度上各有什么差别？
   - 为什么判定放在执行接触点，而不是下单前扫全簿？（给出 §2.1 的过度拦截反例。）
   - FOK 的预检为什么必须懂策略（可达流动性），而 POST-ONLY 的预检可以继续对账户失明？两个预检的"安全方向"分别是什么？
   - 失败的 FOK 为什么必须做到零副作用？如果一个实现"先成交一部分、再发现不够、最后回滚"，会在哪里出问题？
   - 为什么"预检通过 ⇒ 执行必然履约"这个等价性依赖单写者假设（回调不得重入改簿）？
   - 改价为什么必须保留 `account`？不保留会造成什么后果？（对着 `ModifyKeepsAccountAndType` 讲。）
   - 全额成交为什么没有终态事件？"事件 = 市场可观察事实"这个口径如果换成"事件 = 订单生命周期"，契约会怎么变？
   - `SubscriptionId` 为什么单调不复用？构造一个"复用会出事"的场景。
   - 事件为什么必须同步发完再返回（`submit` 返回后，"世界已知"是可以依赖的事实）？异步化会把什么复杂度带进来（预告 W5）？
   - 一笔 CancelOldest 的 IOC 连撞三笔同价自己的挂单，事件流长什么样？outcome 是什么？

**③ 理想效果**：44 全绿，且上述问题能口头回答。

**④ 答案**：无（口试题的答案就是你对代码的理解）。

---

## 任务 7 ｜ 内存/UB 门禁 + 差分对拍全绿 + 提交

**① 目标**

1. 差分对拍全绿（40 种子 × 400 步，两策略各半）；
2. ASan + UBSan + LSan 下 235 个全绿；
3. 提交。

**② 为什么这周的门禁更重**

W4 新增了两个"释放后使用"的温床：`cancel()` 先删后读（发布点顺序错）、事件分发期间的订阅集合生命周期（subscribe/unsubscribe churn）。普通 Debug 构建下这些问题可能完全沉默——内存门禁与生命周期压力用例（`MatchingW4.Lifetime*`）就是为你把它们变成必然可见的失败而存在的。

**③ 提示**

1. 先跑差分与全量：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/tests/maelstrom_tests --gtest_filter='Seeds/MatchingDifferentialW4.*'
```

2. 再跑内存/UB 门禁（脚本会用 ASan+UBSan+LSan 重编库与全部测试，只读源码、只写 /tmp）：

```bash
scripts/run_sanitizers.sh                    # 全部（235 个）
scripts/run_sanitizers.sh 'MatchingW4.*'     # 只跑 W4 语义 + 生命周期
scripts/run_sanitizers.sh 'MatchingW4.Lifetime*'
```

3. 差分报告怎么读（照着 §3 的三元组定位）：

| 报告 | 最可能的病灶 |
|---|---|
| 事件数不一致 | 发布点漏发/多发（Market/IOC 取消；CN 与取消事件重复） |
| 第 k 个事件不一致 | 字段填错（Rested 的 price/account）或顺序错（CO 撤销位置） |
| SubmitResult 不一致 | CN 余量挂回簿（resting 错）或 outcome 手工特判错 |
| FOK 出现 `Cancelled + filled>0` / 差异集中在 `type=Fok` 的 step | 预检按"簿上有多少"而非"可达多少"判定（§2.5）；或预检/执行对 STP 的判定不一致 |
| size / best / find / quantity_at 不一致 | 簿状态维护错（余量没带账户、撤销没清干净） |
| cancel 返回值不一致 | 撤单路径 |

4. **提交**（先确认在你的 W4 分支上）：

```bash
git add -A
git commit -m "W4: event bus + self-match prevention (cancel-newest/cancel-oldest)"
```

**④ 理想效果**

```
# 普通 Debug 构建
[==========] 235 tests from 10 test suites ran.
[  PASSED  ] 235 tests.

# 内存/UB 门禁（ASan+UBSan+LSan）
scripts/run_sanitizers.sh
[  PASSED  ] 235 tests.               ← 无 sanitizer 报告 = 内存干净、无 UB
```

**⑤ 答案**：无（这一步是验收与提交）。

---

## 契约速查（调试对照表）

> 本节是速查，不是学习材料。每一条的动机都在 §1/§2。测试红了，先来这里定位"违反了哪条"，再回去看它为什么长这样。

**类型清单**

| 名字 | 位置 | 一句话 |
|---|---|---|
| `AccountId` | `types.h` | 强类型账户 id；除 0 外任何 int64 |
| `Order::account{0}` | `order.h` | 主体标识；追加在 `type` 之后 |
| `SelfMatchPolicy{CancelNewest, CancelOldest}` | `matching_engine.h` | STP 策略；默认 CancelNewest |
| `EventKind{Trade, OrderRested, OrderCanceled, OrderRejected}` | `matching_engine.h` | 四类可观察事实 |
| `Event` | `matching_engine.h` | 八字段，除 kind 外全有默认值 |
| `SubscriptionId = uint64_t` | `matching_engine.h` | 从 1 递增、不复用 |
| `EventBus` | `matching_engine.h` | subscribe/unsubscribe/publish/subscriber_count |
| `OrderBook::for_each_maker` | `order_book.h` | 只读、按价格时间优先遍历限价内挂单（FOK 预检用） |

**Event 字段语义**（`{0}` = 无意义，保持默认）

| 字段 | Trade | OrderRested | OrderCanceled | OrderRejected |
|---|---|---|---|---|
| `order_id` | 主动单 | 挂簿单 | 被取消单 | 被拒单 |
| `account` | 主动方 | 该单 | 该单 | 该单 |
| `side` | 主动方向 | 该单 | 该单 | 该单 |
| `counterparty_id` | 被动单 | `{0}` | `{0}` | `{0}` |
| `price` | 成交价（被动价） | 挂单价 | `{0}` | `{0}` |
| `quantity` | 成交量 | 挂簿量 | 取消量 | `{0}` |
| `reason` | None | None | None | 拒绝原因 |

**发布点（7 处）**：拒绝（5 原因收口一个出口）/ 每笔成交 / 余量挂簿 / Market-IOC 余量取消 / STP-CN 砍余量 / STP-CO 撤挂单 / `cancel` 成功。全额成交**无**终态事件。

**事件顺序**：实际发生顺序（成交与 CO 撤销按撮合顺序交错）+ 终态事件（若有）最后；同步发完再返回；多订阅按注册顺序。

**STP 规则**

| | CancelNewest | CancelOldest |
|---|---|---|
| 触发 | 循环内、交叉后、吃单前；`account != 0 && 队头账户相同`（只看队头） | 同左 |
| 被取消 | 主动单余量 | 队头挂单（整单） |
| 已成交部分 | 保留 | 保留 |
| 对手挂单 | 不动 | 被撤销，撮合继续（可连撤） |
| 余量 | **不挂簿**（否则簿交叉） | 不受影响（继续吃 / 可能挂簿） |
| outcome | `Cancelled`（既有规则自动落） | 由既有规则决定 |
| 事件 | 1 个 OrderCanceled（主动单） | 每撤一笔 1 个 OrderCanceled |

**FOK 预检（可达流动性，§2.5）**

| 策略 | 可达量 | 预检通过 ⇔ |
|---|---|---|
| CN | 价格时间优先累计他人量，**撞上第一笔自己的挂单即停止** | 执行时不会接触自己的挂单，必然全额成交 |
| CO | 限价内全部他人量（跳过自己的继续累计） | 执行时撤退自己的 Maker 后必然全额成交 |

未通过 ⇒ `Rejected{FokNotFillable}` + **零副作用**（一条 OrderRejected 事件之外什么都没有）。
FOK 永不部分成交、永不挂簿。POST-ONLY 预检保持价格口径（账户盲）。

**改价**：`modify(id, price, qty)` 重挂时保留 `account`/`type`，否则该单从此不受 STP 保护。

**不变的 W3 部分**：校验顺序与五种原因码；POST-ONLY 预检账户盲；成交价=被动价；outcome 判定规则；`set_trade_handler` 通道。

---

## 完成标准、复盘与迁移练习

**完成标准（逐条可检查）**

- [ ] `ctest --test-dir build`：235/235 通过（151 旧 + 84 新）。
- [ ] `scripts/run_sanitizers.sh`：235/235，无 sanitizer 报告。
- [ ] 口试自检的 13 个问题能对着代码回答。
- [ ] `git log` 上有一条 W4 提交。

**复盘（用三句话写进笔记）**

1. 本周新增的能力：____。
2. 最反直觉的一个设计决定是 ____，理由是 ____。
3. 我踩过的最大一个坑是 ____，它属于（正确性 / 内存 / 契约 / 构建）里的哪一类。

**迁移练习（不要求写代码，想清楚即可）**

1. 加第五种策略 **Decrement**（双方各自减量到"刚好不交叉"）：要改哪些函数？事件流会多出什么、会不会出现新的事件种类？簿上/在途的状态分别怎么收尾？
2. 给 `OrderCanceled` 增加一个"取消原因"字段（外部撤单 / STP / IOC 余量）：需要动哪些地方？差分 oracle 怎么同步？契约（测试）要新增什么？
3. 把 `Event` 从固定结构体换成 `std::variant`：`EventBus`、订阅端代码、写日志的消费者各会变成什么样？哪些现有测试要改？换来的是什么、代价是什么？

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
    AccountId account{0}; // 0 = 未设置账户，永不触发自成交防护（§2.2）
};
```

**为什么**：追加 + 默认值 + 不加构造函数，三条纪律与 W3 任务 1 相同；`{0}` 比省略默认值更明确——聚合初始化少写字段时走"未设置"而非不确定值。

### A2 — 任务 2：matching_engine.h 增补

类型定义（节选自 §1.3 / §1.6 / §2.6 的契约，逐字段照抄）：

```cpp
// 自成交防护策略（仅当双方账户相同且非 0 时生效，§2.6）。
enum class SelfMatchPolicy : std::uint8_t {
    CancelNewest, // 撞上自己的挂单：主动单余量取消，挂单不动
    CancelOldest, // 撞上自己的挂单：撤掉该挂单，撮合继续
};

// 事件种类 = 四类市场可观察事实（§1.2）。
enum class EventKind : std::uint8_t {
    Trade,
    OrderRested,
    OrderCanceled,
    OrderRejected,
};

using SubscriptionId = std::uint64_t;

// 事件。与 kind 无关的字段保持默认值（{0} / None）。
struct Event {
    EventKind            kind;
    maelstrom::OrderId   order_id{0};
    maelstrom::AccountId account{0};
    maelstrom::Side      side = maelstrom::Side::Sell;
    maelstrom::OrderId   counterparty_id{0};
    maelstrom::Price     price{0};
    maelstrom::Quantity  quantity{0};
    RejectReason         reason = RejectReason::None;
};

// 多订阅者事件总线：同步分发，按订阅注册顺序逐一回调（§1.4）。
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
    void set_self_match_policy(SelfMatchPolicy policy);          // 只影响其后的 submit
    [[nodiscard]] SubscriptionId subscribe(EventBus::Handler handler);
    bool unsubscribe(SubscriptionId id);

private:
    EventBus        bus_;
    SelfMatchPolicy policy_ = SelfMatchPolicy::CancelNewest;
```

需要 `#include <vector>`。

### A3 — 任务 3/4：簿的只读遍历原语 + matching_engine.cpp 增量

先加簿原语（`order_book.h` / `order_book.cpp`），再改引擎（`matching_engine.cpp`）。

**簿原语**（声明与实现，均不改任何数据结构）：

```cpp
// order_book.h（类内，放在 matchable_quantity 之后）
// 按价格时间优先顺序【只读】遍历 side 侧限价内的挂单；visit 返回 false 立即停止。
void for_each_maker(Side side, std::optional<Price> limit,
                    const std::function<bool(const Order &)> &visit) const;

// order_book.cpp
void OrderBook::for_each_maker(Side side, std::optional<Price> limit,
                               const std::function<bool(const Order &)> &visit) const {
    if (!impl_ || !visit) return;
    if (side == Side::Buy) {                        // 买盘降序：最高买价优先
        for (const auto &[price, level] : impl_->bids) {
            if (limit.has_value() && price < limit->get()) break;
            for (const OrderNode *n = level.head; n != nullptr; n = n->next) {
                if (!visit(n->order)) return;
            }
        }
    }
    else {                                          // 卖盘升序：最低卖价优先
        for (const auto &[price, level] : impl_->asks) {
            if (limit.has_value() && price > limit->get()) break;
            for (const OrderNode *n = level.head; n != nullptr; n = n->next) {
                if (!visit(n->order)) return;
            }
        }
    }
}
```

**引擎增量**（`matching_engine.cpp`；以下是**增量视角**，完整结构仍是你 W3 写的 `submit`）：

```cpp
// 1) 拒绝收口：五个校验分支全走这里（发布点 1/7）
auto reject = [&](RejectReason r) {
    Event ev{};
    ev.kind     = EventKind::OrderRejected;
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
// ... 价格校验与 POST-ONLY 预检同 W3（账户盲，§2.5），走 reject(...)

// 3) FOK 预检：STP 感知的"可达流动性"判定（§2.5）——替换 W3 的 matchable_quantity 单查
const std::optional<Price> limit = is_market ? std::nullopt : std::optional<Price>{o.price};
if (o.type == OrderType::Fok) {
    // 3a) 快速排除：限价内总量（含自己的）是可达量的上界，不足则必然不可成交
    if (book_.matchable_quantity(opp, limit).get() < o.quantity.get()) {
        return reject(RejectReason::FokNotFillable);
    }
    // 3b) 精查：按价格时间优先累计"当前策略下真正能吃到"的量（纯读，零副作用）
    std::int64_t       reachable = 0;
    const std::int64_t need      = o.quantity.get();
    book_.for_each_maker(opp, limit, [&](const Order &maker) {
        if (o.account.get() != 0 && maker.account == o.account) {
            if (policy_ == SelfMatchPolicy::CancelNewest) {
                return false;   // CN：撞上自己的挂单余量必被砍，可达流动性到此为止
            }
            return true;        // CO：自己的挂单会被撤掉，不提供成交也不阻塞
        }
        reachable += maker.quantity.get();
        return reachable < need; // 凑够即停
    });
    if (reachable < need) return reject(RejectReason::FokNotFillable);
}

bool self_killed = false; // STP-CN：主动单余量被砍，禁止挂簿（§2.4）
while (remaining > 0) {
    const auto best = (opp == Side::Sell) ? book_.best_ask() : book_.best_bid();
    if (!best.has_value()) break;
    if (!is_market && !crosses(o.side, o.price, best->price)) break;

    // STP：执行接触点判定，只看队头（§2.6）
    if (o.account.get() != 0 && best->account == o.account) {
        if (policy_ == SelfMatchPolicy::CancelNewest) {
            Event ev{};                                  // 发布点 5/7
            ev.kind = EventKind::OrderCanceled;
            ev.order_id = o.orderId; ev.account = o.account; ev.side = o.side;
            ev.quantity = Quantity{remaining};
            bus_.publish(ev);
            self_killed = true;
            break;
        }
        Event ev{};                                      // 发布点 6/7（CO 可连发）
        ev.kind = EventKind::OrderCanceled;
        ev.order_id = best->orderId; ev.account = best->account; ev.side = best->side;
        ev.quantity = best->quantity;
        bus_.publish(ev);
        book_.cancel(best->orderId);
        continue; // 不消耗 remaining；同价下一笔可能又是自己的
    }

    // ... W3 原有：take / consume_best / Trade 事件（发布点 2/7）/ handler_(t)
}

const bool can_rest = !self_killed && (o.type == OrderType::Limit || o.type == OrderType::PostOnly);
if (remaining > 0 && can_rest) {
    Order r = o; r.quantity = Quantity{remaining};       // 账户随余量入簿（§2.6）
    book_.add(r);
    rest = Quantity{remaining};
    Event ev{};                                          // 发布点 3/7
    ev.kind = EventKind::OrderRested;
    ev.order_id = r.orderId; ev.account = r.account; ev.side = r.side;
    ev.price = r.price; ev.quantity = rest;
    bus_.publish(ev);
}
else if (remaining > 0 && !self_killed) {
    Event ev{};                                          // 发布点 4/7：Market/IOC 余量取消
    ev.kind = EventKind::OrderCanceled;                  // （!self_killed 防止与 CN 重复）
    ev.order_id = o.orderId; ev.account = o.account; ev.side = o.side;
    ev.quantity = Quantity{remaining};
    bus_.publish(ev);
}
// outcome 组装与 W3 完全相同：CN 自然落 Cancelled，不需要新分支（§2.4）
```

`cancel()`（先捕获字段，再撤，再发事件——顺序不能反）：

```cpp
bool MatchingEngine::cancel(OrderId id) {                // 发布点 7/7
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
- STP-CN 不需要新的 outcome 分支——W3 的规则（`resting == 0` 且 `filled < original` → `Cancelled`）自动覆盖它；你要做的只是用 `self_killed` 把余量挡在挂簿之外（§2.4）。
- `else if (remaining > 0 && !self_killed)` 是 Market/IOC 的终态事件出口；`!self_killed` 防止与 CN 的取消事件重复。
- 每轮循环要么撤一笔（簿缩小）、要么吃一笔（remaining 缩小）、要么 break——CO 的连撤必然终止（§2.4）。
- FOK 预检的两条核心性质：**只读**（失败 ⇒ 零副作用，簿连一笔 Maker 都不能少）与**等价**（通过 ⇒ 撮合阶段必然填满——CN 下不会接触自己的挂单、CO 下撤退自己的 Maker 后他人量足够；这正是 §2.5 定义的可达流动性在起作用）。注意 `for_each_maker` 里的 `return false` 是"停止遍历"，不是"失败"——失败与否由累计出的 `reachable < need` 决定。

### A4 — 任务 5：CMake 改动

`tests/CMakeLists.txt`（仅此一处）：

```cmake
add_executable(maelstrom_tests
    test_order_book.cpp
    test_order_book_w2.cpp
    test_matching_engine_w3.cpp
    test_matching_engine_w4.cpp)
```

`naive_matching_engine_w4.h` 会被测试源自动 include，无需登记；库目标没有任何新增。

### A5 — 差分与门禁的报告怎么读

- 差分失败信息自带现场：`seed=... step=... policy=... type=... side=... acct=...`；
  事件不一致时还给 `第 k 个事件` 与双方 `kind`（0=Trade、1=OrderRested、2=OrderCanceled、3=OrderRejected）。
- `policy=0` 是 CancelNewest、`policy=1` 是 CancelOldest——先看"是不是只红一类策略"。
- 把 seed/step 记下来，写个小 main 重放前 step+1 步（RNG 消费顺序照抄测试）即可二分定位到具体操作。
- 门禁报 `heap-use-after-free`：优先查 `cancel()` 的"先捕获后删"顺序与发布点时序；
  报 `leaked`：优先查 subscribe 了没退订、`Order` 复制路径有没有裸资源。

---

## 附：本周边界（做什么、不做什么）

- **做**：`AccountId` 与订单账户、自成交防护两策略（执行接触点判定）、
  `Event`/`EventBus`（多订阅、退订、注册顺序、同步分发）、引擎七处发布点、
  **FOK 的策略感知可达流动性预检**（含簿的只读遍历原语）、
  **改价保留 `account`/`type`**、W3 全量回归、事件流参与随机差分、内存/UB 门禁。
- **不做（留给后续）**：
  - 分发期间订阅/退订、多线程分发与并发安全 → W5（无锁队列 + 线程隔离）；
  - 更多 STP 策略（Decrement / RejectIncoming / CancelBoth）→ 扩展模块（迁移练习 1）；
  - `std::variant` 事件、事件持久化/回放、订阅优先级 → 扩展模块（迁移练习 3）；
  - 订单改价触发重撮合（提示：改价 = 撤单事件 + 新挂单事件？）；
  - 可达流动性预检的性能优化（每笔 FOK 扫描可成交前缀；账户索引/缓存是扩展题）→ W7；
  - 低延迟分发（内联小函数、静态多播）→ W6/W7。

> 一句话记住 W4 的定位：**W3 的引擎只对调用者负责；W4 的引擎对市场负责——
> 发生的每件事都广播给所有关心的人（EventBus），不该发生的事当场拦下（STP），
> 而这一切在 40 个随机种子下与参考实现逐字节一致。**

---

## 参考资料

阅读目的已标注；按需读，不必通读。

- cppreference：`std::function`（https://en.cppreference.com/w/cpp/utility/functional/function）——
  订阅回调的存储载体；重点看"空对象调用未定义/抛出"，理解我们"未设置 handler 时先判空"的写法。
- cppreference：`std::variant`（https://en.cppreference.com/w/cpp/utility/variant）——
  §1.3 讨论过的替代事件表示；读 `std::visit` 部分，做迁移练习 3 时对照。
- cppreference：`std::optional`（https://en.cppreference.com/w/cpp/utility/optional）——
  §2.2 讨论过的账户表示替代方案。
- cppreference：aggregate initialization（https://en.cppreference.com/w/cpp/language/aggregate_initialization）——
  "为什么追加字段 + 默认值不破坏旧初始化"的规则出处（C++17 口径）。
- 交易所 STP 机制：在 Nasdaq / CME 官网搜索 "Self-Match Prevention"——
  行业做法与策略空间（§2.3 的五种策略即由此归纳），作为领域背景阅读，不要求实现。
- 《Trading and Exchanges》— Larry Harris（PROJECT_PLAN 已列）——
  自成交/洗售在市场微观结构里的定位（§0.3 的领域动机）。
