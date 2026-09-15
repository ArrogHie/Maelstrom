# W2 手写任务单 —— 从 O(n) 订单簿到 O(1) price-time 订单簿

> 这是一份**练习册**，不是参考手册。你的 W1 只剩一个冒烟测试和一个 `std::vector` 占位实现；
> 本周要把它升级成真正的限价订单簿。这份文档带你有节奏地走完，每个任务都按固定四段组织：
>
> **① 问题** → 你此刻面对的具体困难或现象（先想清楚"为什么要有这一步"）
> **② 提示** → 怎么动手的线索（不是完整答案，卡住再往下读）
> **③ 理想效果** → 完成后应该能看到什么（命令输出 / 测试颜色）
> **④ 答案** → 放在文末附录，编号对应。写不出或写完对照，**别复制粘贴，手打一遍**。
>
> 规则重申：所有代码由你手写进 `include/` 和 `src/`。`tests/` 里已备好两组**裁判测试**（AI 隔离编写，只做验收、不进你的实现目录）：
> 约 18 个语义单元测试 `OrderBookW2.*` + 40 组随机差分对拍 `Differential/*`。你的实现正确与否由它们判定。
> 语义契约见下方"判据"，你的代码和它必须逐字一致。

---

## 0. 先认识问题：W2 到底在解决什么

W1 交付后，代码长这样（`src/order_book.cpp`）：订单存进一个 `std::vector`，`best_bid()` 用 `std::max_element` 把整张表扫一遍。它能跑，但有四个真问题：

| # | 问题 | 后果 |
|---|---|---|
| 1 | **慢**：查 best、撤单都是 O(n) 全扫 | 每秒千万笔的撮合场景直接不可用；W7 要和 `std::map` 基线做性能对比，现在这个结构没资格参赛 |
| 2 | **内存裸奔**：裸 `Impl* impl` + 手写 `new/delete`，还 `= delete` 了拷贝**和**移动 | 不能移动、析构手动管理，与 W1 指南自己说好的"W2 上 RAII"相悖 |
| 3 | **接口残缺**：没有 `modify`（改量/改价）、`find`、`quantity_at`；`add` 返回 `void`，拒绝订单时**调用者毫不知情** | 一个撮合引擎必须知道"单到底挂没挂上"，还要能改单撤单 |
| 4 | **无人验收**：只有一个冒烟测试 | price-time priority（同价谁先到谁优先）这类核心语义没有任何测试盯着 |

W2 的数据结构目标，就是让上面三个操作从"全扫"变成常数时间：

| 操作 | W1 现状 | W2 目标 | 靠什么 |
|---|---|---|---|
| 按订单号找到一笔单 | O(n) 扫 | O(1) 摊还 | `unordered_map<OrderId, 节点*>` |
| 同价位排队（谁先到谁优先） | vector 顺序隐含 | O(1) 尾插 | 每价位一条**双向链表** |
| 查 best bid / ask | O(n) 取 max/min | O(1) | 买卖各一张**按价排序**的 map，`begin()` 即最优 |
| 撤单 / 改价 | O(n) | O(1) 摊还 | 指针直接摘链 |

本周你动手做的每件事，都能对应回这张表。

---

## 1. 判据：测试裁判的规则（先通读，别跳）

下面的规则就是 `OrderBookW2.*` 里手写断言 + `Differential/*` 里参考簿共同强制的东西。
任何一条没满足，测试就会红；红的时候回来对照这条。

- **add**：`price <= 0` 或 `quantity <= 0` → 拒绝返回 `false`；`orderId` 已存在 → 拒绝返回 `false`（原单不受影响）。否则挂到 (side, price) 价位**队尾**、发一个新时间戳 seq、返回 `true`。被撤掉的 id 之后**可以重新 add**（按新单重新排队）。
- **cancel**：id 不在簿 → `false`；在簿 → 整单移除（摘链、更新价位累计量、空价位删档、从 map 删除）、返回 `true`。同一 id 撤两次，第二次 `false`。
- **modify(id, 新数量)**：id 不存在或 `新数量 <= 0` → `false`；否则**同价改量**，该单在队列里的位置不变（**时间优先级保留**）。
- **modify(id, 新价, 新数量)**：id 不存在、`新价 <= 0`、`新数量 <= 0` → `false`。
  - 新价 == 原价：等价上一条（只改量，保留优先级）。
  - 新价 != 原价：**撤单 + 重挂**——从原价位链上摘下来（让位给后面的人），以新价/新量排到目标价位**队尾**，发**新 seq（失去原时间优先级）**。side 不变。
- **best_bid()**：买盘最高价位里 **seq 最小**（最早到达）的那笔的副本；买盘空 → `nullopt`。**只含买盘**。
- **best_ask()**：卖盘最低价位里 seq 最小的那笔；卖盘空 → `nullopt`。
- **find(id)**：该单当前快照（含改过的价/量）；不存在 → `nullopt`。
- **quantity_at(p)**：价格 p 上的挂单总量，**买卖两侧都算**（买 100 挂 5、卖 100 挂 3 → 返回 8）；没挂单 → `Quantity{0}`。
- **size()**：在簿订单**笔数**（买卖合计），单位是"单"不是"数量"。
- **允许买单和卖单同价共存**（买价 ≥ 卖价不冲突），撮合是 W3 的事。

> 哪条规则没看懂，就在测试里搜它的断言，看测试怎么理解它。判据以测试为准。

---

## 2. 任务概览（做完一个打一个勾）

| # | 任务 | 改哪个文件 | 预计 |
|---|---|---|---|
| 1 | 给价格类型装排序能力 | `include/Maelstrom/types.h` | 15 min |
| 2 | 给订单簿换一张"稳定的脸"（接口 + RAII） | `include/Maelstrom/order_book.h` | 30 min |
| 3 | 把裁判挂进构建，把红灯读成 TODO | `tests/CMakeLists.txt` | 15 min |
| 4 | 搭内部结构 + 两条指针原语（最难点） | `src/order_book.cpp` | 2–3 h |
| 5 | 把 9 个公开方法拼出来，让测试转绿 | `src/order_book.cpp` | 3–5 h |
| 6 | 差分对拍全绿 + 口试自检 + 提交 | 只读不写 | 1–2 h |

---

## 任务 1 ｜ 给价格类型装上排序能力

**① 问题**

你在心里规划订单簿时，想用 `std::map<Price, ...>` 把价格排好序、让"最高买价/最低卖价"直接就是 `begin()`。
但 `Price` 是 `StrongType`，而模板 `StrongType` 目前只实现了 `==` 和 `!=`。`std::map` 排序依赖比较器——默认 `std::less` 要求 `Price` 支持 `<`，买盘用的 `std::greater` 要求支持 `>`。
这些运算符现在不存在，所以一旦写出 `std::map<Price, ...>`，编译器就会报 `no match for 'operator<'`。**任务 1 不先做，任务 4 编不过。**

**② 提示**

1. 打开 `include/Maelstrom/types.h`，找到 `StrongType` 模板里那两个比较运算符，它们在类里面、能直接读私有成员 `value`。
2. 照着 `==` 的样子再写 4 个成员函数：`<` `>` `<=` `>=`。每个都是一行 `return value < other.value;` 这种。
3. 签名要带 `const`（常量订单簿要能比较价格）+ `const StrongType&` 参数（不拷贝）+ `noexcept`（比较永不抛异常）。返回 `bool`。
4. 会不会因为多了 `<`，导致 `Price` 能和 `int64_t` 互相比较？不会——构造函数是 `explicit`，编译器拒绝把 `100` 隐式当成 `Price`，类型防线没破。
5. 这是给**模板**加运算符：一改，`Price`/`Quantity`/`OrderId` 三个强类型同时获得全套排序能力。注意此时还没人用这些新运算符，所以现有代码不该有任何变化。

**③ 理想效果**

```bash
cmake --build build -j    # 成功，无新增警告
ctest --test-dir build     # W1 冒烟测试仍绿（你没破坏任何东西）
```

**④ 答案**：附录 A1。

---

## 任务 2 ｜ 给订单簿换一张"稳定的脸"（头文件接口 + RAII）

**① 问题**

现在 `OrderBook` 用裸 `Impl* impl` + 手写 `new/delete`，还通过 `= delete` 同时禁用了拷贝**和**移动。
带来的现实麻烦：① 对象不能放进任何要求可移动的场合；② 内存手动管理，W1 指南自己在"9. order_book.h"里写了"W2 换 `std::unique_ptr`"；
③ 接口缺 `modify` / `find` / `quantity_at`，`add` 还返回 `void`——**拒绝了一笔单，调用者什么都不知道**，这在撮合场景里没法忍。

**② 提示**

目标：头文件只当"合同"——声明方法、不暴露内部；类变得可移动、不可拷贝；每个会失败的操作都把成败告诉调用者。

1. 把 `Impl* impl = nullptr;` 换成 `std::unique_ptr<Impl> impl_;`（记得 `#include <memory>`）。
2. `add` 的返回类型从 `void` 改成 `bool`（成功与否要让调用者知道）。
3. 新增方法的声明：`modify`（两版）、`find`、`quantity_at`。它们的确切签名看下方"判据"和附录 A2。
4. pImpl + unique_ptr 的"五条规则"，一次写全：
   - 拷贝构造/赋值：`= delete`（`unique_ptr` 不可拷，你也确实不想让订单簿被复制）；
   - 移动构造/赋值：声明成 `noexcept`，**只声明不实现**（实现放 .cpp，见任务 4）；
   - **析构函数：只声明（`~OrderBook();`），不写 `= default`**——因为 `unique_ptr<Impl>` 析构时需要 `Impl` 的完整定义，而 `Impl` 只声明在头文件里。析构必须定义在 .cpp（`struct OrderBook::Impl` 定义好之后）才能编译。这是 pImpl 的经典约束，忘了它你会看到诡异的 `invalid application of 'sizeof' to incomplete type` 报错。
5. 头文件 include `<cstddef>`（size_t）、`<memory>`、`<optional>`，以及 `"Maelstrom/order.h"`（提供 Order/Price/Quantity/OrderId/Side）。
6. **先别管 .cpp**——接口先行。改完头文件直接 `cmake --build build -j`，你会看到 .cpp 大量报错：这是"新合同 vs 老实现"对不上，**正好**，错误清单就是任务 5 的 TODO。

**③ 理想效果**

`order_book.cpp` 报错，且报错内容能让你列出如下清单（这就是你接下来要实现的东西）：

- `add` 签名不匹配（老实现返回 void）→ 需要按新签名重写；
- `modify` / `find` / `quantity_at` 未声明 → 需要新增实现；
- `~OrderBook` 未定义 / 析构问题 → 需要在 .cpp 给出定义。

**④ 答案**：附录 A2（头文件全文）。

---

## 任务 3 ｜ 把裁判挂进构建，把红灯读成 TODO

**① 问题**

测试裁判已经写在 `tests/` 里了，但构建系统还不知道它们的存在——`tests/CMakeLists.txt` 目前只编译 `test_order_book.cpp` 这一个测试源。
不把它们挂进来，你写了半天都不知道对没对。

**② 提示**

1. 打开 `tests/CMakeLists.txt`，找到 `add_executable(maelstrom_tests ...)` 那一行，把新测试源 `test_order_book_w2.cpp` 加进源列表。
   （`naive_order_book.h` 是头文件，会被测试源 `#include`，不需要写进 CMake。）
2. 改了 CMake 文件必须重新配置，然后再构建：

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DMAELSTROM_BUILD_TESTS=ON -DMAELSTROM_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

3. 现在预期是**红灯一片**：编译错（头/实现不一致）+ 链接错（`~OrderBook` 未定义、`modify` 未定义……）。把报错逐条读出来、抄成一份 TODO 清单——那就是任务 5 要消灭的敌人。
4. 认一下常用的两把"尺子"：

```bash
./build/tests/maelstrom_tests --gtest_filter='OrderBookW2.*'        # 只看语义单测
./build/tests/maelstrom_tests --gtest_filter='OrderBookW2.Cancel*'  # 只看撤单相关
./build/tests/maelstrom_tests --gtest_filter='Differential/RandomOpSequence/7'  # 只看某个随机种子
ctest --test-dir build --output-on-failure                          # 全量，失败打详情
```

**③ 理想效果**

你能说出这句话："W2 要我实现的 = `add`（改签名）、`cancel`、`modify` ×2、`find`、`quantity_at`、析构/移动，外加内部数据结构。" 构建目前红着——这是**预期内的红**，是起跑线。

**④ 答案**：附录 A3（改动很小，只一行）。

---

## 任务 4 ｜ 搭内部结构 + 两条指针原语（本周最难点，值得慢）

**① 问题**

旧实现是"一张 vector 全扫"，问题在任务 0 说过了。W2 要用业界标准"三件套"：

```
                 ┌─────────────────────────────────────┐
  O(1) 找单       │  unordered_map<OrderId, OrderNode*>  │  每个订单一个节点
                 └──────────────────┬──────────────────┘
                 ┌──────────────────┴──────────────────┐
  O(1) 同价排队    │  每价位一条双向链表: OrderNode 串起来  │  头=最早到达，尾=最晚到达
                 │  Level{ head, tail, total(累计量) }   │
                 └──────────────────┬──────────────────┘
                 ┌──────────────────┴──────────────────┐
  O(1) 查 best    │  买盘 map<Price, Level, greater>     │  begin() = 最高买价那档
                 │  卖盘 map<Price, Level>（价低在前）    │  begin() = 最低卖价那档
                 └─────────────────────────────────────┘
```

难的不是概念，而是**指针**：一个订单节点同时活在"哈希表"和"链表"两处；增删时要同步维护好几样东西，接错一根指针就是悬垂/成环/崩溃，而且 bug 常常藏在看起来很对的代码里。所以任务 4 先把结构搭对、把"**追加**"和"**摘除**"两条原语写对，任务 5 的 9 个方法就全是"查一下 → 调一条原语"的一两行薄代码。

**② 提示**

*结构部分（照着自己的理解敲，参考在 A4）：*

1. 一个价位一个 `Level`，一条订单一个 `OrderNode`。节点要能自己回答三个问题：我的订单是什么？我在这条链上前面/后面是谁？我属于哪个价位档？所以：
   `OrderNode { Order order; OrderNode* prev; OrderNode* next; Level* level; std::uint64_t seq; }`
   `seq` 是"到达时间戳"：同价位的先后，本质是 seq 大小，链表只是把"头 = seq 最小"这件事 O(1) 地维护出来。
2. `Level { OrderNode* head; OrderNode* tail; Quantity total; }`。`total` 是这一档的累计挂单量——`quantity_at` 要 O(1) 就靠它，不能每次现数链表。
3. `OrderBook::Impl` 里放：上面两张 `std::map`、一个 `std::unordered_map`、一个 `std::uint64_t next_seq = 1;`（每次发单 `next_seq++`）。
4. `unordered_map` 需要 `std::hash<OrderId>`，而 StrongType 没给。写一个小的局部哈希对象（A4 有，3 行）。
5. **`bids` 和 `asks` 是两种不同的 `std::map` 类型**（比较器一个是 `std::greater`、一个是默认 `std::less`）——所以**不能用 `(side == Buy) ? bids : asks` 这种三元表达式**（编不过），"按 side 选容器"要么 if/else 写两遍，要么抽成按 `PriceMap` 参数化的模板函数。先撞上这个编译错是好事。
6. 两个"指针安全"论证，面试会问，先想明白：
   - `map<Price, Level>` 里，只要某个价位不删，`&it->second` 的地址**稳定**（map 节点堆分配、不搬移）。我们只在该价位**变空**时才 erase 它——那时已没有任何节点还指着它。所以 `OrderNode::level` 存裸指针安全。
   - `orders` 里为什么不存节点本体而存 `OrderNode*`？因为节点本体同时被链表指；它的地址必须稳定。节点是 `new` 出来的，地址天然稳定；`unordered_map` 只存指针，rehash/扩容不影响指针指向的元素（节点型容器只失效迭代器、不失效元素指针）。记住这条，任务 6 的口试题里要用。

*两条原语（先在纸上把四种情况画出来，再敲）：*

7. **追加**（把一笔订单作为新单挂到某价位的队尾，发新 seq）：
   - 在对应 map 里 `try_emplace` 出 `Level*`（不存在就新建这一档）；
   - `new` 一个 `OrderNode`，填订单、seq、level；
   - 队尾若有人：`tail->next = node; node->prev = tail;` 再更新 `tail = node`；没人（新档第一个）：`head = tail = node;`
   - `level->total += quantity`。
8. **摘除单节点 `unlink(node)`**（从链表摘下来，先不释放、先不删档）——这是全周最容易写错的地方。原则一句话：**谁指着我进来，我就让它指向我原来指着的**。展开就是四种情况：
   - node 有前驱 → `node->prev->next = node->next;`
   - 没有前驱 → node 是队头 → `level->head = node->next;`
   - node 有后继 → `node->next->prev = node->prev;`
   - 没有后继 → node 是队尾 → `level->tail = node->prev;`
   - 然后 `level->total -= quantity`，把 `node->prev/node->next` 置空。
   在纸上画"头、尾、中间、唯一一个"四种链表，各走一遍再写。
9. **移除整单 `remove(node)`** = `unlink` + 若该档链表已空就从对应 map `erase` 掉这个价格 + `orders.erase(id)` + `delete node`。**顺序不能乱：先摘干净 → 再删空档 → 最后释放**。哪一步顺序反了，就是"用了已释放的指针"的未定义行为，随机对拍会用它崩给你看。

*让项目先"能跑、能看全红"：*

10. 写完结构和原语后，把 9 个公开方法先写成**最简占位（stub）**，保证链接通过——注意这些 stub 是"临时脚手架"，不是答案：

```cpp
// 临时占位：让项目先跑起来，看"全红起点"。任务 5 逐个替换成真实现。
OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}
OrderBook::~OrderBook() = default;
OrderBook::OrderBook(OrderBook&&) noexcept = default;
OrderBook& OrderBook::operator=(OrderBook&&) noexcept = default;

bool OrderBook::add(Order) { return false; }          // stub
bool OrderBook::cancel(OrderId) { return false; }     // stub
// modify ×2 → false；best_bid/best_ask/find → std::nullopt；
// quantity_at → Quantity{0}；size → 0
```

   这时跑 `--gtest_filter='OrderBookW2.*'`，18 个测试应该**全红**（W1 的 Smoke 因为 stub `add` 返回 false 也会红，正常）。这就是你的起跑线：接下来每替换一个真方法，红就少一批。

**③ 理想效果**

- 项目能编译、能链接，`OrderBookW2.*` 全红（stub 状态），你能说出每条原语在代码里的位置和它服务的操作。
- 手动走查：`unlink` 对"头/尾/中间/唯一节点"四种情况各发生了什么，你能不看代码讲出来。

**④ 答案**：附录 A4（结构 + 原语 + 特殊成员的参考实现）。

---

## 任务 5 ｜ 把 9 个公开方法拼出来，让测试转绿

**① 问题**

结构有了，原语有了。现在每个公开方法都只是"查一下状态 → 决定调哪条原语 / 返回什么"，但各自藏着几个小决策：拒绝条件、查哪一侧、返回值、优先级保不保留。逐个替换 stub，让红灯分批变绿。

**② 提示**

按下面的顺序做（每个方法都薄，难点在任务 4；顺序是按依赖排的，推荐）：

1. **find(id)**：`orders.find(id)`，没有 → `nullopt`；有 → 返回节点里 `order` 的**副本**（值拷贝，不是引用）。
2. **size()**：`orders.size()`——是订单笔数，不是累计数量。
3. **add(o)**：先查两条拒绝（`price<=0 || qty<=0`；id 已存在）→ `false`；否则**追加**（走任务 4 原语，内部会发新 seq、排到队尾），返回 `true`。拒绝时**不能**先 `new` 再丢。
4. **best_bid / best_ask**：各自查对的 map：`if (map.empty()) return nullopt; return map.begin()->second.head->order;`——为什么这就是答案？map 首元素是价格最优的那档；`head` 是该档最早到达（seq 最小）的单；最优价 × 最早到 = price-time 最优。所以 **best 是 O(1)**。
5. **quantity_at(p)**：**两边 map 都 `find(p)`**，把两档 `total` 加起来（同价允许买卖共存）；某一侧没这档就是 0。注意 `total` 是 `Quantity`，相加要 `.get()` 取 `int64_t` 再包回 `Quantity{...}`。
6. **cancel(id)**：`find_node` 没有 → `false`；有 → `remove(node)` → `true`。remove 内部干了摘链+删档+删 map+释放，所以 cancel 本体只有 3 行。
7. **modify(id, newQty)**：`newQty <= 0` 或 id 不存在 → `false`；否则同价改量：`level->total += (新 - 旧)` 的**差值**，覆盖 `node->order.quantity`。**seq 和链表位置一律不动**——时间优先级保留。别直接 `total = 新量`，要算差值。
8. **modify(id, newPrice, newQty)**：`newPrice<=0 || newQty<=0` 或 id 不存在 → `false`；若 `newPrice == 原价`，退化成第 7 条（只改量）；否则 = **撤单 + 重挂**：把该单的 `side`/`id` 记下来，`remove` 掉旧节点，再以新价/新量走 `add` 的追加路径。追加会发**新 seq** → 它自动排到新价位的**队尾**，时间优先级在结构上就丢了，一行特判都不用写。
9. 每替换完一组 stub 就跑一次看红变少：

```bash
cmake --build build -j
./build/tests/maelstrom_tests --gtest_filter='OrderBookW2.*'   # 目标：全绿
```

   若还有红，用 filter 把范围缩小到出问题的那类（如 `Cancel*`、`ModifyPrice*`），结合判据逐条对。

*容易踩的坑（红了先回来对照这里）：*

- [ ] `total` 忘记同步（add/cancel/modify 三处都要动）→ `quantity_at` 红。
- [ ] 空档没 erase → map 里残留 `Level{head=nullptr}`，`best_bid` 一解引用就崩。
- [ ] `delete` 之后还碰节点 / 删了 map 里不存在的 id → 悬垂或断言崩。
- [ ] 同价改量把 total 直接赋成新量而不是加差值。
- [ ] StrongType 不能直接做算术：`total.get() + qty.get()` 取数算完再包回，别让 `int64_t` 隐式缩窄成 `size_t`（`-Wconversion` 会报）。
- [ ] modify 改价时把 side 改了 / 把 id 换了。
- [ ] 判据里的拒绝条件少判一条（比如忘了 `price == 0` 也要拒）。

**③ 理想效果**

```bash
./build/tests/maelstrom_tests --gtest_filter='OrderBookW2.*'    # 18 个全绿
./build/tests/maelstrom_tests --gtest_filter='OrderBook.SmokeAddAndSize'  # W1 冒烟仍绿
ctest --test-dir build --output-on-failure                        # 除 Differential 外全绿
```

**④ 答案**：附录 A5。

---

## 任务 6 ｜ 差分对拍全绿 + 口试自检 + 提交

**① 问题**

18 个手写用例都绿，说明每个规则在**精心构造**的场景下都对了，但随机长序列下呢？你改错链表顺序、在某个犄角旮旯漏更新 `total`，手写用例不一定抓得到。
差分对拍（differential testing）就是干这个的：`Differential/*` 用 40 个随机种子，每个种子生成 600 步随机的 add/cancel/modify，**每步之后**把你实现的簿和一份"慢速参考簿"（O(n)、简单到不容易写错，同样按判据实现）的五个观察量——`size / best_bid / best_ask / find(id) / quantity_at`——逐一比对。任何一步不一致，测试就红并告诉你现场。

**② 提示**

1. 跑全量：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

   或者直接 `./build/tests/maelstrom_tests`（会先跑 18 个单测再跑 40 组对拍）。
2. 对拍失败时，输出会告诉你是哪个 seed、第几步、哪个操作、哪个观察量不一致。单点复现 + 缩小现场：

```bash
./build/tests/maelstrom_tests --gtest_filter='Differential/RandomOpSequence/7'
```

   seed=7。用这个最小现场，在纸上回放那一步，或临时打印你的簿内部状态来定位。
3. 失败类型 → 病因速查：
   - `quantity_at 不一致` → `Level.total` 没同步（改量/撤单/改价三处都查）。
   - `best_bid/best_ask 不一致` → 空档没删 / 删错了侧 / `head` 指错 / 改价没重排队。
   - `find 不一致` → cancel 后 map 里残留了已释放节点（悬垂），查删除顺序。
   - `返回值不一致` → 拒绝条件或成功条件与判据对不上。
4. **口试自检**（不用写代码，但要能对着代码讲清楚，写进笔记）：
   - 为什么 `best_bid()` 是 O(1)？为什么 `cancel` 摊还 O(1) 而里面有一次 O(log L)？（L = 价格档数）
   - "改价 = 撤单 + 重挂"丢时间优先级，是在**哪一行**结构上保证的？
   - 为什么 `map` 里的 `Level` 可以被节点用裸指针安全指着？（提示：什么时候 erase、erase 前有没有人还指着它）
   - 如果我把 `orders` 从 `unordered_map` 换成 `std::vector<std::pair<...>>`，会破坏哪个不变量？（提示：节点地址稳定性）
5. **提交**（记得先确认在 `W2_order_book` 分支）：

```bash
git add -A
git commit -m "W2: full limit order book with price-time priority"
```

**③ 理想效果**

```
[==========] 58 tests ran.   ← 18 单测 + 40 差分，全 PASSED
```

`OrderBookW2.*` 与 `Differential/RandomOpSequence/1..40` 全绿，W1 `SmokeAddAndSize` 仍绿，commit 已打。

---

## 附录：参考答案

> 顺序是**你写完一个任务 → 对答案 → 合上 → 自己再敲一遍**。手打不是为了敲对，是为了让手指记得指针的走向。每份答案都给出"为什么这么做"的一句话，方便你对照自己的思路。

### A1 — 任务 1：types.h 补全比较运算符

加在 `==` / `!=` 后面（同一份模板内）：

```cpp
constexpr bool operator<(const StrongType &other) const noexcept { return value < other.value; }
constexpr bool operator>(const StrongType &other) const noexcept { return value > other.value; }
constexpr bool operator<=(const StrongType &other) const noexcept { return value <= other.value; }
constexpr bool operator>=(const StrongType &other) const noexcept { return value >= other.value; }
```

为什么这么写：成员运算符能读私有 `value`；`const` 成员 + `const&` 参数保证常量对象也能比较且不拷贝；`noexcept` 因为内置比较不抛异常。
一行改动，`Price`/`Quantity`/`OrderId` 从此可排序——`std::map` / `std::greater` / 参考簿里的 `a.price > b.price` 全靠它。

### A2 — 任务 2：order_book.h（头文件全文）

```cpp
#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "Maelstrom/order.h"

namespace maelstrom {

// 限价订单簿：price-time priority。内部用 pImpl 隐藏，见 src/order_book.cpp。
class OrderBook {
public:
    OrderBook();
    ~OrderBook();                                    // 定义放 .cpp（pImpl 需要完整 Impl）

    OrderBook(const OrderBook&)            = delete; // 不可拷贝
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept;                 // 可移动；定义放 .cpp
    OrderBook& operator=(OrderBook&&) noexcept;

    // 挂单：price/quantity > 0 且 orderId 不重复才成功。失败返回 false。
    bool add(Order o);

    // 整单撤销；不在簿返回 false。
    bool cancel(OrderId id);

    // 同价改量：保留时间优先级；id 不存在或 newQuantity<=0 返回 false。
    bool modify(OrderId id, Quantity newQuantity);

    // 改量改价：价格变化 = 撤单+重挂（新 seq、排新价位队尾，失去原优先级）；
    // 价格不变则等价上一重载。非法参数或 id 不存在返回 false。
    bool modify(OrderId id, Price newPrice, Quantity newQuantity);

    // 买盘最优价位的队头单（该价位最早到达）；买盘空返回 nullopt。
    [[nodiscard]] std::optional<Order> best_bid() const;
    // 卖盘最优价位的队头单；卖盘空返回 nullopt。
    [[nodiscard]] std::optional<Order> best_ask() const;

    // 在簿订单当前快照；不存在返回 nullopt。
    [[nodiscard]] std::optional<Order> find(OrderId id) const;

    // 某价格挂单总量（买卖两侧都算）；无挂单返回 Quantity{0}。
    [[nodiscard]] Quantity quantity_at(Price p) const;

    // 在簿订单总笔数（买卖合计）。
    [[nodiscard]] std::size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace maelstrom
```

为什么这么写：拷贝删除 + 移动保留是 `unique_ptr` 成员的必然要求；析构声明在头、定义在 .cpp 是 pImpl 铁律（`unique_ptr<不完整类型>` 的内联析构编不过）。

### A3 — 任务 3：tests/CMakeLists.txt（只改一行）

```cmake
add_executable(maelstrom_tests test_order_book.cpp test_order_book_w2.cpp)
```

`naive_order_book.h` 会被 `test_order_book_w2.cpp` 自动 `#include`，无需登记。改完必须重新 `cmake -B build ...` 再 `cmake --build build -j`。

### A4 — 任务 4：结构 + 原语 + 特殊成员（order_book.cpp 的上半部分）

```cpp
#include "Maelstrom/order_book.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>

namespace maelstrom {

namespace {
struct OrderIdHash {                          // StrongType 没有 std::hash，自备
    std::size_t operator()(OrderId id) const noexcept {
        return std::hash<std::int64_t>{}(id.get());
    }
};
} // namespace

// 一笔订单 = 公共字段 + 链表指针 + 所属价位 + 时间戳
struct OrderNode {
    Order         order;
    OrderNode    *prev = nullptr;             // 同价位链表前驱
    OrderNode    *next = nullptr;             // 同价位链表后继
    struct Level *level = nullptr;            // 挂在哪个价位（摘链/删档要用）
    std::uint64_t seq = 0;                    // 时间优先级：档内 seq 最小者先成交
};

// 一个价位 = 一条双向链表 + 累计量
struct Level {
    OrderNode *head = nullptr;                // 最早到达
    OrderNode *tail = nullptr;                // 最晚到达
    Quantity   total{0};                      // 档内累计量（quantity_at 的 O(1) 来源）
};

struct OrderBook::Impl {
    using Orders = std::unordered_map<OrderId, OrderNode*, OrderIdHash>;
    Orders orders;                                        // id -> 节点（I1）
    std::map<Price, Level, std::greater<Price>> bids;     // 买盘，价高在前，begin=best
    std::map<Price, Level> asks;                          // 卖盘，价低在前
    std::uint64_t next_seq = 1;                           // 单调发号

    OrderNode* find_node(OrderId id) {
        auto it = orders.find(id);
        return it == orders.end() ? nullptr : it->second;
    }

    // ---- 原语一：追加（一笔“全新”订单到 (side, price) 队尾，发 seq）----
    // bids 与 asks 类型不同，用模板统一；调用处按 side 分派。
    template <typename PriceMap>
    OrderNode* append_into(PriceMap &m, Order order, std::uint64_t seq) {
        auto      it = m.try_emplace(order.price).first;  // 没有该档就新建
        Level    *lvl = &it->second;
        OrderNode *n = new OrderNode;
        n->order = std::move(order);
        n->seq   = seq;
        n->level = lvl;
        if (lvl->tail != nullptr) {           // 队尾有人：接在后面
            lvl->tail->next = n;
            n->prev         = lvl->tail;
            lvl->tail       = n;
        } else {                               // 该档第一个：头尾都是它
            lvl->head = lvl->tail = n;
        }
        lvl->total = Quantity{lvl->total.get() + n->order.quantity.get()};
        orders.emplace(n->order.orderId, n);
        return n;
    }

    OrderNode* append(Order o, std::uint64_t seq) {       // 按 side 分派到对应 map
        if (o.side == Side::Buy) return append_into(bids, std::move(o), seq);
        return append_into(asks, std::move(o), seq);
    }

    // ---- 原语二：摘链（从所在价位链表摘下来；不释放、不删档）----
    void unlink(OrderNode *n) {
        Level *lvl = n->level;
        if (n->prev != nullptr) {             // 有前驱：让前驱跳过自己
            n->prev->next = n->next;
        } else {                              // 没前驱 = 队头
            lvl->head = n->next;
        }
        if (n->next != nullptr) {             // 有后继：让后继跳过自己
            n->next->prev = n->prev;
        } else {                              // 没后继 = 队尾
            lvl->tail = n->prev;
        }
        n->prev = nullptr;                    // 清理，防悬垂
        n->next = nullptr;
        lvl->total = Quantity{lvl->total.get() - n->order.quantity.get()};
    }

    // ---- 原语三：整单移除（顺序：摘链 -> 空档删档 -> 删 map -> 释放）----
    void remove(OrderNode *n) {
        unlink(n);
        if (n->level->head == nullptr) {      // 这一档空了：从价位 map 删掉
            if (n->order.side == Side::Buy) {
                bids.erase(n->order.price);
            } else {
                asks.erase(n->order.price);
            }
        }
        orders.erase(n->order.orderId);       // 从哈希表摘掉
        delete n;                             // 最后才释放
    }
};
```

### A5 — 任务 5：9 个公开方法 + 特殊成员（order_book.cpp 的下半部分）

```cpp
// ---- 特殊成员（pImpl 铁律：都必须定义在 Impl 完整之后）----
OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}
OrderBook::~OrderBook() = default;
OrderBook::OrderBook(OrderBook&&) noexcept = default;
OrderBook& OrderBook::operator=(OrderBook&&) noexcept = default;

bool OrderBook::add(Order o) {
    if (o.price.get() <= 0 || o.quantity.get() <= 0) return false;   // 拒绝条件 1
    if (impl_->orders.find(o.orderId) != impl_->orders.end()) return false;  // 拒绝条件 2
    impl_->append(std::move(o), impl_->next_seq++);   // 内部发新 seq、排队尾
    return true;
}

bool OrderBook::cancel(OrderId id) {
    OrderNode *n = impl_->find_node(id);
    if (n == nullptr) return false;
    impl_->remove(n);
    return true;
}

bool OrderBook::modify(OrderId id, Quantity newQuantity) {
    if (newQuantity.get() <= 0) return false;
    OrderNode *n = impl_->find_node(id);
    if (n == nullptr) return false;
    Level *lvl = n->level;
    // 同价改量：total 加“差值”；seq 与链表位置不动 → 时间优先级保留
    lvl->total = Quantity{lvl->total.get() - n->order.quantity.get() + newQuantity.get()};
    n->order.quantity = newQuantity;
    return true;
}

bool OrderBook::modify(OrderId id, Price newPrice, Quantity newQuantity) {
    if (newPrice.get() <= 0 || newQuantity.get() <= 0) return false;
    OrderNode *n = impl_->find_node(id);
    if (n == nullptr) return false;
    if (n->order.price == newPrice) {         // 价格没变：退化成只改量
        return modify(id, newQuantity);
    }
    Order moved = n->order;                   // 留下 side / id
    impl_->remove(n);                         // 撤单（含删档，见 remove）
    moved.price    = newPrice;
    moved.quantity = newQuantity;
    impl_->append(std::move(moved), impl_->next_seq++);  // 重挂：新 seq -> 新档队尾
    return true;
}

std::optional<Order> OrderBook::best_bid() const {
    if (impl_->bids.empty()) return std::nullopt;
    return impl_->bids.begin()->second.head->order;   // 最高价档的队头 = 最优价×最早到
}

std::optional<Order> OrderBook::best_ask() const {
    if (impl_->asks.empty()) return std::nullopt;
    return impl_->asks.begin()->second.head->order;
}

std::optional<Order> OrderBook::find(OrderId id) const {
    auto it = impl_->orders.find(id);
    if (it == impl_->orders.end()) return std::nullopt;
    return it->second->order;                 // 返回快照副本
}

Quantity OrderBook::quantity_at(Price p) const {
    std::int64_t sum = 0;                     // 两侧都查：同价买卖允许共存
    auto bb = impl_->bids.find(p);
    if (bb != impl_->bids.end()) sum += bb->second.total.get();
    auto ba = impl_->asks.find(p);
    if (ba != impl_->asks.end()) sum += ba->second.total.get();
    return Quantity{sum};
}

std::size_t OrderBook::size() const {
    return impl_->orders.size();              // 笔数，非数量
}
```

对照你自己的实现检查三处最容易漏的：`modify` 里 `total` 是否用**差值**更新；`remove` 的顺序是否是"摘链 → 删空档 → 删 map → delete"；`best_bid/best_ask` 是否判了 `empty()`。
