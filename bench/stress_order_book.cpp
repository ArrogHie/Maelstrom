// W2 订单簿压力测试 / 效率诊断（AI 编写，隔离在 bench/，不参与 include/ 与 src/）。
//
// 目的：用"极限数据"验证 docs/W2_ORDER_BOOK_GUIDE.md 声称的复杂度，
//       并量化 vs O(n) 朴素基线的差距。
//
// ⚠️ 必须用 Release 构建（Debug 下数字没有意义）：
//   clang++ -std=c++17 -O3 -DNDEBUG -march=native -I include \
//           src/order_book.cpp bench/stress_order_book.cpp -o /tmp/stress
//   /tmp/stress all            # 全量（默认 N=10^6）
//   /tmp/stress scaling        # 复杂度扫描（验证 O(1) 是否成立）
//   /tmp/stress baseline 1000000   # 与 O(n) 朴素簿对比
//   /tmp/stress latency 1000000    # 单次操作延迟分位数
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <memory_resource>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <x86intrin.h>

#include "Maelstrom/order_book.h"

using namespace maelstrom;

namespace {

// ---------------------------------------------------------------- 基础设施

// 阻止编译器把"没有副作用"的被测调用优化掉
inline void sink(const void *p) { asm volatile("" : : "g"(p) : "memory"); }
inline void sink_opt(const std::optional<Order> &o) { asm volatile("" : : "g"(&o) : "memory"); }
inline void barrier() { asm volatile("" : : : "memory"); }

// TSC 频率：cycles / ns。用 steady_clock 标定。
double cycles_per_ns() {
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = __rdtsc();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.10) {
    }
    const std::uint64_t c1 = __rdtsc();
    const double        sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return static_cast<double>(c1 - c0) / sec / 1.0e9;
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 单次操作延迟采样（纳秒）
struct Latency {
    std::vector<std::uint64_t> cycles;
    explicit Latency(std::size_t n) { cycles.reserve(n); }

    void push_start_end(std::uint64_t c0, std::uint64_t c1) { cycles.push_back(c1 - c0); }

    void report(const char *name, double cpn) const {
        if (cycles.empty()) return;
        std::vector<std::uint64_t> v = cycles;
        std::sort(v.begin(), v.end());
        const auto at = [&](double p) {
            auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
            return static_cast<double>(v[idx]) / cpn;
        };
        std::printf("   %-22s p50=%7.1f  p90=%7.1f  p99=%9.1f  p99.9=%9.1f  max=%10.1f  (ns)\n",
                    name, at(0.50), at(0.90), at(0.99), at(0.999),
                    static_cast<double>(v.back()) / cpn);
    }
};

void print_throughput(const char *name, long long n, double secs) {
    std::printf("   %-30s N=%-10lld  %9.2f ms  %8.1f ns/op  %10.3f M ops/s\n", name, n,
                secs * 1e3, secs / static_cast<double>(n) * 1e9,
                static_cast<double>(n) / secs / 1e6);
}

Order mk(std::int64_t id, std::int64_t px, std::int64_t qty, Side s) {
    return Order{Price{px}, OrderId{id}, Quantity{qty}, s};
}

// ------------------------------------------------- O(n) 朴素基线（做对照）
// 刻意用最简单、最直白的写法：vector 全扫。用来量化"数据结构"带来的差距。
class NaiveBook {
public:
    bool add(Order o) {
        entries_.push_back(o);
        return true;
    }
    bool cancel(OrderId id) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->orderId == id) {
                entries_.erase(it);
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] std::optional<Order> best_bid() const {
        const Order *best = nullptr;
        for (const Order &o : entries_) {
            if (o.side != Side::Buy) continue;
            if (best == nullptr || o.price > best->price) best = &o;
        }
        if (best == nullptr) return std::nullopt;
        return *best;
    }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    std::vector<Order> entries_;
};

// ------------------------------------------------------------------ 场景

// S1: 同价极深队列（极限：所有单堆在同一个价位）—— 考验链表尾插 O(1)
double s_add_deep(long long n) {
    OrderBook book;
    const double t0 = now_s();
    for (long long i = 0; i < n; ++i) {
        sink_opt(book.best_bid()); // 干扰项：每单后都查一次 best，确保簿真的"活着"
        book.add(mk(i + 1, 10000, 10, Side::Buy));
    }
    const double dt = now_s() - t0;
    sink_opt(book.best_bid());
    return dt;
}

// S2: 海量独立价位（极限：每单价位都不同）—— 考验 std::map 插入 O(log L)
double s_add_levels(long long n) {
    OrderBook book;
    const double t0 = now_s();
    for (long long i = 0; i < n; ++i) {
        book.add(mk(i + 1, 10000 + i, 10, Side::Buy));
    }
    const double dt = now_s() - t0;
    sink_opt(book.best_bid());
    return dt;
}

// S3: 先进先出撤单（撤队头，深队列）—— 考验摘头 O(1)
double s_cancel_fifo(long long n) {
    OrderBook book;
    for (long long i = 0; i < n; ++i) book.add(mk(i + 1, 10000, 10, Side::Buy));
    const double t0 = now_s();
    for (long long i = 0; i < n; ++i) {
        sink(book.best_bid().has_value() ? &book : nullptr);
        book.cancel(OrderId{i + 1});
    }
    return now_s() - t0;
}

// S4: 随机撤单 —— 考验摘中间节点（前驱/后继四种情况都走到）
double s_cancel_random(long long n, std::uint32_t seed) {
    OrderBook book;
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (long long i = 0; i < n; ++i) {
        book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
        ids.push_back(OrderId{i + 1});
    }
    std::mt19937 rng(seed);
    std::shuffle(ids.begin(), ids.end(), rng);
    const double t0 = now_s();
    for (const OrderId &id : ids) book.cancel(id);
    return now_s() - t0;
}

// S5: 同价改量（不动优先级，只加差值）—— 最轻的热路径
double s_modify_qty(long long n) {
    OrderBook book;
    for (long long i = 0; i < n; ++i) book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
    const double t0 = now_s();
    for (long long i = 0; i < n; ++i) {
        book.modify(OrderId{i + 1}, Quantity{20});
    }
    return now_s() - t0;
}

// S6: 改价（= 撤单 + 重挂，含一次释放 + 一次分配）—— 最重的热路径
double s_modify_price(long long n) {
    OrderBook book;
    for (long long i = 0; i < n; ++i) book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
    const double t0 = now_s();
    for (long long i = 0; i < n; ++i) {
        book.modify(OrderId{i + 1}, Price{20000 + (i % 1000)}, Quantity{10});
    }
    return now_s() - t0;
}

// S7: best_bid/best_ask 纯查询热路径 —— 应当 O(1)
double s_query_best(long long n) {
    OrderBook book;
    for (long long i = 0; i < n; ++i) {
        book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
        book.add(mk(n + i + 1, 11000 + (i % 1000), 10, Side::Sell));
    }
    const long long q = n; // 查询次数 = 订单数
    const double    t0 = now_s();
    for (long long i = 0; i < q; ++i) {
        sink_opt(book.best_bid());
        sink_opt(book.best_ask());
    }
    return now_s() - t0;
}

// S8: quantity_at 查询（两次 map 查找，O(log L)）
double s_query_quantity(long long n) {
    OrderBook book;
    for (long long i = 0; i < n; ++i) book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
    const double t0 = now_s();
    for (long long i = 0; i < n; ++i) {
        Quantity q = book.quantity_at(Price{10000 + (i % 1000)});
        sink(&q);
    }
    return now_s() - t0;
}

// S9: 随机混合负载（增删改混合，模拟真实订单流）
double s_mixed(long long ops, std::uint32_t seed) {
    OrderBook                  book;
    std::vector<OrderId>       live;
    std::mt19937               rng(seed);
    live.reserve(static_cast<std::size_t>(ops) / 2 + 1);
    std::int64_t next_id = 1;

    const double t0 = now_s();
    for (long long k = 0; k < ops; ++k) {
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 55 || live.empty()) { // 55% 挂单
            const std::int64_t px  = 10000 + static_cast<std::int64_t>(rng() % 200);
            const Side         s   = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderId      id{next_id++};
            if (book.add(mk(id.get(), px, 10, s))) live.push_back(id);
        }
        else if (roll < 85) { // 30% 撤单
            const auto idx = static_cast<std::size_t>(rng() % live.size());
            book.cancel(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        }
        else if (roll < 95) { // 10% 改量
            const auto idx = static_cast<std::size_t>(rng() % live.size());
            book.modify(live[idx], Quantity{5 + static_cast<std::int64_t>(rng() % 20)});
        }
        else { // 5% 改价
            const auto idx = static_cast<std::size_t>(rng() % live.size());
            const std::int64_t px = 10000 + static_cast<std::int64_t>(rng() % 200);
            book.modify(live[idx], Price{px}, Quantity{10});
        }
    }
    const double dt = now_s() - t0;
    sink_opt(book.best_bid());
    return dt;
}

// S10: 单次操作延迟分位数（rdtscp 逐次计时）
void s_latency(long long n, double cpn) {
    std::printf("\n[延迟分位数] 逐次操作 rdtscp 计时，N=%lld\n", n);

    { // 测量地板：空操作，说明计时本身的开销
        Latency lat(static_cast<std::size_t>(n));
        unsigned aux = 0;
        for (long long i = 0; i < n; ++i) {
            const std::uint64_t c0 = __rdtscp(&aux);
            barrier();
            const std::uint64_t c1 = __rdtscp(&aux);
            lat.push_start_end(c0, c1);
        }
        lat.report("[地板] 空操作", cpn);
    }

    { // add（深队列，热价档：只有 1 个价位，无 map 插入）
        OrderBook book;
        Latency   lat(static_cast<std::size_t>(n));
        unsigned  aux = 0;
        for (long long i = 0; i < n; ++i) {
            Order o = mk(i + 1, 10000, 10, Side::Buy);
            const std::uint64_t c0 = __rdtscp(&aux);
            book.add(o);
            const std::uint64_t c1 = __rdtscp(&aux);
            lat.push_start_end(c0, c1);
        }
        lat.report("add (同价档已存在)", cpn);
    }

    { // add（新价位：要走 map 插入 + 节点分配）
        OrderBook book;
        Latency   lat(static_cast<std::size_t>(n));
        unsigned  aux = 0;
        for (long long i = 0; i < n; ++i) {
            Order o = mk(i + 1, 10000 + i, 10, Side::Buy);
            const std::uint64_t c0 = __rdtscp(&aux);
            book.add(o);
            const std::uint64_t c1 = __rdtscp(&aux);
            lat.push_start_end(c0, c1);
        }
        lat.report("add (新价位)", cpn);
    }

    { // best_bid
        OrderBook book;
        for (long long i = 0; i < n; ++i) book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
        Latency  lat(static_cast<std::size_t>(n));
        unsigned aux = 0;
        for (long long i = 0; i < n; ++i) {
            const std::uint64_t c0 = __rdtscp(&aux);
            const auto          r  = book.best_bid();
            const std::uint64_t c1 = __rdtscp(&aux);
            sink_opt(r);
            lat.push_start_end(c0, c1);
        }
        lat.report("best_bid", cpn);
    }

    { // cancel（随机顺序，摘中间节点）
        OrderBook book;
        std::vector<OrderId> ids;
        for (long long i = 0; i < n; ++i) {
            book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
            ids.push_back(OrderId{i + 1});
        }
        std::mt19937 rng(12345);
        std::shuffle(ids.begin(), ids.end(), rng);
        Latency  lat(static_cast<std::size_t>(n));
        unsigned aux = 0;
        for (long long i = 0; i < n; ++i) {
            const std::uint64_t c0 = __rdtscp(&aux);
            book.cancel(ids[static_cast<std::size_t>(i)]);
            const std::uint64_t c1 = __rdtscp(&aux);
            lat.push_start_end(c0, c1);
        }
        lat.report("cancel (随机)", cpn);
    }
}

// S11: 复杂度扫描——O(1) 的话 ns/op 应该基本不随 N 增长
void s_scaling(double cpn) {
    std::printf("\n[复杂度扫描] 若为 O(1)，ns/op 应随 N 基本持平\n");
    std::printf("   %-10s %14s %14s %14s %14s\n", "N", "add_deep", "add_levels", "cancel_rand",
                "best_bid");

    const long long sizes[] = {10000, 100000, 1000000, 4000000};
    for (long long n : sizes) {
        const double a = s_add_deep(n) / static_cast<double>(n);
        const double b = s_add_levels(n) / static_cast<double>(n);

        OrderBook            book;
        std::vector<OrderId> ids;
        ids.reserve(static_cast<std::size_t>(n));
        for (long long i = 0; i < n; ++i) {
            book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
            ids.push_back(OrderId{i + 1});
        }

        // 注意：best_bid 必须在"有单的书"上测。先前版本把它放在撤空之后，
        // 量到的是空簿早退（~2.7ns），是假数据。
        const long long q = 1000000;
        const double    t_q = now_s();
        for (long long i = 0; i < q; ++i) sink_opt(book.best_bid());
        const double qq = (now_s() - t_q) / static_cast<double>(q);

        std::mt19937 rng(7);
        std::shuffle(ids.begin(), ids.end(), rng);
        const double t_c = now_s();
        for (const OrderId &id : ids) book.cancel(id);
        const double c = (now_s() - t_c) / static_cast<double>(n);

        std::printf("   %-10lld %11.1f ns %11.1f ns %11.1f ns %11.1f ns\n", n, a * 1e9, b * 1e9,
                    c * 1e9, qq * 1e9);
    }
    (void)cpn;
}

// S12: 与 O(n) 基线对比（同样的操作，看差距）
void s_baseline(long long n) {
    std::printf("\n[vs O(n) 基线] N=%lld （best_bid / cancel 各 %lld 次）\n", n, n / 10);
    const long long reps = n / 10;

    OrderBook fast;
    NaiveBook slow;
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (long long i = 0; i < n; ++i) {
        const Order o = mk(i + 1, 10000 + (i % 1000), 10, Side::Buy);
        fast.add(o);
        slow.add(o);
        ids.push_back(OrderId{i + 1});
    }

    { // best_bid 查询
        const double t0 = now_s();
        for (long long i = 0; i < reps; ++i) sink_opt(fast.best_bid());
        const double fast_s = now_s() - t0;

        const double t1 = now_s();
        for (long long i = 0; i < reps; ++i) sink_opt(slow.best_bid());
        const double slow_s = now_s() - t1;

        std::printf("   best_bid : 本实现 %7.1f ns/op  |  朴素 %10.1f ns/op  |  快 %.0f×\n",
                    fast_s / static_cast<double>(reps) * 1e9,
                    slow_s / static_cast<double>(reps) * 1e9, slow_s / fast_s);
    }

    { // cancel（取前 reps 个 id，随机顺序）
        std::mt19937 rng(99);
        std::shuffle(ids.begin(), ids.end(), rng);
        std::vector<OrderId> ids_fast(ids.begin(), ids.begin() + reps);
        std::vector<OrderId> ids_slow(ids.begin(), ids.begin() + reps);

        const double t0 = now_s();
        for (const OrderId &id : ids_fast) fast.cancel(id);
        const double fast_s = now_s() - t0;

        const double t1 = now_s();
        for (const OrderId &id : ids_slow) slow.cancel(id);
        const double slow_s = now_s() - t1;

        std::printf("   cancel   : 本实现 %7.1f ns/op  |  朴素 %10.1f ns/op  |  快 %.0f×\n",
                    fast_s / static_cast<double>(reps) * 1e9,
                    slow_s / static_cast<double>(reps) * 1e9, slow_s / fast_s);
    }
}

// 用 malloc 分配器自己的记账统计，避免 RSS 受 glibc arena 复用/归还的干扰
long malloc_inuse_kb() {
    const struct mallinfo2 mi = mallinfo2();
    return static_cast<long>(mi.uordblks / 1024);
}

// 不变量校验（只用公共 API）。返回 "" 表示通过，否则返回失败描述。
std::string verify_invariants(OrderBook &book, const std::vector<OrderId> &live) {
    // 每个在簿 id 必须能找到
    for (const OrderId &id : live) {
        if (!book.find(id).has_value()) return "find() 丢失了在簿订单";
    }
    if (book.size() != live.size()) {
        return "size()=" + std::to_string(book.size()) + " 与预期 " + std::to_string(live.size()) +
               " 不符";
    }
    // best_bid 必须是买盘最高价；best_ask 必须是卖盘最低价
    std::optional<std::int64_t> max_bid, min_ask;
    for (const OrderId &id : live) {
        const auto o = book.find(id);
        if (!o.has_value()) continue;
        if (o->side == Side::Buy) {
            if (!max_bid.has_value() || o->price.get() > *max_bid) max_bid = o->price.get();
        }
        else {
            if (!min_ask.has_value() || o->price.get() < *min_ask) min_ask = o->price.get();
        }
    }
    const auto bb = book.best_bid();
    const auto ba = book.best_ask();
    if (bb.has_value() != max_bid.has_value()) return "best_bid 存在性与买盘不符";
    if (bb.has_value() && bb->price.get() != *max_bid) return "best_bid 不是最高买价";
    if (ba.has_value() != min_ask.has_value()) return "best_ask 存在性与卖盘不符";
    if (ba.has_value() && ba->price.get() != *min_ask) return "best_ask 不是最低卖价";

    // quantity_at 必须等于该价位所有在簿订单数量之和
    std::vector<std::int64_t> prices;
    for (const OrderId &id : live) {
        const auto o = book.find(id);
        if (o.has_value()) prices.push_back(o->price.get());
    }
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
    for (std::int64_t p : prices) {
        std::int64_t sum = 0;
        for (const OrderId &id : live) {
            const auto o = book.find(id);
            if (o.has_value() && o->price.get() == p) sum += o->quantity.get();
        }
        if (book.quantity_at(Price{p}).get() != sum) {
            return "quantity_at(" + std::to_string(p) + ") 与实际挂单量不符";
        }
    }
    return {};
}

// S13: 极端负载耐力测试 —— 大批量增删改后校验不变量是否仍然成立
void s_endurance(long long n) {
    std::printf("\n[耐力/不变量] N=%lld 混合操作后校验结构一致性\n", n);
    OrderBook            book;
    std::vector<OrderId> live;
    live.reserve(static_cast<std::size_t>(n));
    std::mt19937 rng(2026);
    long long    next_id = 1;
    const int    price_levels = 200;

    const double t0 = now_s();
    for (long long k = 0; k < n; ++k) {
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 60 || live.empty()) {
            const std::int64_t px = 10000 + static_cast<std::int64_t>(rng() % price_levels);
            const Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderId id{next_id++};
            if (book.add(mk(id.get(), px, 10, s))) live.push_back(id);
        }
        else if (roll < 88) {
            const auto idx = static_cast<std::size_t>(rng() % live.size());
            if (book.cancel(live[idx])) {
                live[idx] = live.back();
                live.pop_back();
            }
        }
        else if (roll < 96) {
            const auto idx = static_cast<std::size_t>(rng() % live.size());
            book.modify(live[idx], Quantity{1 + static_cast<std::int64_t>(rng() % 100)});
        }
        else {
            const auto idx = static_cast<std::size_t>(rng() % live.size());
            const std::int64_t px = 10000 + static_cast<std::int64_t>(rng() % price_levels);
            book.modify(live[idx], Price{px}, Quantity{10});
        }
    }
    const double dt = now_s() - t0;
    std::printf("   %lld 次混合操作: %.2f ms (%.1f ns/op), 最终在簿 %zu 单\n", n, dt * 1e3,
                dt / static_cast<double>(n) * 1e9, book.size());

    const std::string err1 = verify_invariants(book, live);
    std::printf("   不变量校验(负载后): %s\n", err1.empty() ? "通过 ✓" : err1.c_str());

    // 清空所有单，再查一次是否彻底干净
    for (const OrderId &id : live) book.cancel(id);
    const std::vector<OrderId> empty;
    const std::string err2 = verify_invariants(book, empty);
    std::printf("   不变量校验(清空后): %s (size=%zu, best_bid有值=%d, best_ask有值=%d)\n",
                err2.empty() ? "通过 ✓" : err2.c_str(), book.size(),
                static_cast<int>(book.best_bid().has_value()),
                static_cast<int>(book.best_ask().has_value()));
}

// S14: 分配器对照 —— 预演 W6 自定义 arena 能带来什么（pmr 单调缓冲）
void s_allocator(long long n) {
    std::printf("\n[分配器对照] 每笔 56B 节点，N=%lld\n", n);

    { // glibc malloc：每个节点一次 new + 一次 delete
        const double t0 = now_s();
        {
            std::vector<std::unique_ptr<char[]>> keep;
            keep.reserve(static_cast<std::size_t>(n));
            for (long long i = 0; i < n; ++i) keep.push_back(std::make_unique<char[]>(56));
        }
        const double dt = now_s() - t0;
        std::printf("   glibc new[]+delete[]    %9.1f ms  %8.1f ns/op\n", dt * 1e3,
                    dt / static_cast<double>(n) * 1e9);
    }

    { // pmr 单调缓冲：一次性大块，之后零散分配只是指针 bump
        std::vector<char> buf(static_cast<std::size_t>(n) * 128);
        std::pmr::monotonic_buffer_resource pool(buf.data(), buf.size());
        const double t0 = now_s();
        for (long long i = 0; i < n; ++i) sink(pool.allocate(56));
        const double dt = now_s() - t0;
        std::printf("   pmr 单调缓冲 allocate %9.1f ms  %8.1f ns/op  <-- W6 方向\n", dt * 1e3,
                    dt / static_cast<double>(n) * 1e9);
    }
}

void s_memory(long long n) {
    std::printf("\n[内存占用] 用 malloc 记账统计\n");
    const long base = malloc_inuse_kb();
    {
        OrderBook book;
        for (long long i = 0; i < n; ++i) book.add(mk(i + 1, 10000 + (i % 1000), 10, Side::Buy));
        const long used = malloc_inuse_kb() - base;
        std::printf("   %lld 单（1000 价位）: 堆占用约 %ld KB  => %.1f 字节/单\n", n, used,
                    static_cast<double>(used) * 1024.0 / static_cast<double>(n));
        sink(&book);
    }
    const long base2 = malloc_inuse_kb();
    {
        OrderBook book;
        for (long long i = 0; i < n; ++i) book.add(mk(i + 1, 10000 + i, 10, Side::Buy));
        const long used = malloc_inuse_kb() - base2;
        std::printf("   %lld 单（%lld 价位）: 堆占用约 %ld KB  => %.1f 字节/单\n", n, n, used,
                    static_cast<double>(used) * 1024.0 / static_cast<double>(n));
        sink(&book);
    }
}

long long arg_n(int argc, char **argv, int idx, long long dflt) {
    if (argc > idx) return std::atoll(argv[idx]);
    return dflt;
}

} // namespace

int main(int argc, char **argv) {
    const std::string mode = (argc > 1) ? argv[1] : "all";
    const long long   n    = arg_n(argc, argv, 2, 1000000);
    const double      cpn  = cycles_per_ns();

    std::printf("=== Maelstrom W2 订单簿压力测试 ===\n");
    std::printf("TSC = %.3f cycles/ns   模式=%s   N=%lld\n", cpn, mode.c_str(), n);

    if (mode == "all" || mode == "throughput") {
        std::printf("\n[吞吐] 各场景 ns/op 与 M ops/s\n");
        print_throughput("S1 add 同价深队列", n, s_add_deep(n));
        print_throughput("S2 add 海量独立价位", n, s_add_levels(n));
        print_throughput("S3 cancel FIFO(队头)", n, s_cancel_fifo(n));
        print_throughput("S4 cancel 随机", n, s_cancel_random(n, 42));
        print_throughput("S5 modify 同价改量", n, s_modify_qty(n));
        print_throughput("S6 modify 改价(撤+挂)", n, s_modify_price(n));
        print_throughput("S7 查询 best_bid+ask×2", n, s_query_best(n));
        print_throughput("S8 查询 quantity_at", n, s_query_quantity(n));
        print_throughput("S9 随机混合负载", n, s_mixed(n, 2024));
    }
    if (mode == "all" || mode == "endurance") s_endurance(n);
    if (mode == "all" || mode == "allocator") s_allocator(std::min<long long>(n, 4000000));
    if (mode == "all" || mode == "latency") s_latency(std::min<long long>(n, 1000000), cpn);
    if (mode == "all" || mode == "scaling") s_scaling(cpn);
    if (mode == "all" || mode == "baseline") s_baseline(n);
    if (mode == "all" || mode == "memory") s_memory(std::min<long long>(n, 2000000));

    std::printf("\n完成。\n");
    return 0;
}
