// AI 提供的隔离验收代码（仅供测试差分对拍，不参与生产库）。
//
// NaiveOrderBook —— 语义与 W2 规格逐条一致的“慢速参考簿”(oracle)。
// 实现刻意直白（vector + 全扫，O(n)）：只求“简单到不容易写错”，
// 用来与真实 OrderBook 做 differential testing：
// 随机喂相同操作序列，逐步比对 size / best_bid / best_ask / find / quantity_at。
//
// 语义契约（与 docs/W2_ORDER_BOOK_GUIDE.md 的 A2 一致）：
//  - add:  price/quantity > 0 且 id 不重复 才落簿；否则 false
//  - cancel: 不在簿返回 false
//  - modify(id, qty): 同价改量，保留时间优先级（seq 不变）
//  - modify(id, price, qty): 改价 = 撤单 + 重挂（发新 seq，排到新价位队尾）；
//                            price 与原价相同则等价上一版
//  - best_bid/best_ask: 最优价位的队头单（seq 最小）；对应盘空则 nullopt
//  - quantity_at: 该价格挂单总量（买卖两侧都算）
//  - size: 在簿订单总笔数（买卖合计）
//  - 同价双方向（bid>=ask）可共存，本簿不做撮合
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "Maelstrom/order.h"

namespace maelstrom {
namespace test_support {

class NaiveOrderBook {
public:
    NaiveOrderBook() = default;

    bool add(Order o) {
        if (o.price.get() <= 0 || o.quantity.get() <= 0) return false;
        if (find_it(o.orderId) != entries_.end()) return false;
        entries_.push_back(Entry{std::move(o), next_seq_++});
        return true;
    }

    bool cancel(OrderId id) {
        auto it = find_it(id);
        if (it == entries_.end()) return false;
        entries_.erase(it);
        return true;
    }

    bool modify(OrderId id, Quantity newQuantity) {
        if (newQuantity.get() <= 0) return false;
        auto it = find_it(id);
        if (it == entries_.end()) return false;
        it->order.quantity = newQuantity;
        return true;
    }

    bool modify(OrderId id, Price newPrice, Quantity newQuantity) {
        if (newPrice.get() <= 0 || newQuantity.get() <= 0) return false;
        auto it = find_it(id);
        if (it == entries_.end()) return false;
        if (it->order.price == newPrice) { // 同价改量：保留时间优先级
            it->order.quantity = newQuantity;
        } else {                            // 改价：撤单 + 重挂，发新 seq
            it->order.price = newPrice;
            it->order.quantity = newQuantity;
            it->seq = next_seq_++;
        }
        return true;
    }

    [[nodiscard]] std::optional<Order> best_bid() const { return best(Side::Buy, /*max_price=*/true); }
    [[nodiscard]] std::optional<Order> best_ask() const { return best(Side::Sell, /*max_price=*/false); }

    [[nodiscard]] std::optional<Order> find(OrderId id) const {
        auto it = find_it(id);
        if (it == entries_.end()) return std::nullopt;
        return it->order;
    }

    [[nodiscard]] Quantity quantity_at(Price p) const {
        std::int64_t sum = 0;
        for (const Entry &e : entries_) {
            if (e.order.price == p) sum += e.order.quantity.get();
        }
        return Quantity{sum};
    }

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        Order        order;
        std::uint64_t seq = 0;
    };

    std::vector<Entry>  entries_;
    std::uint64_t       next_seq_ = 1;

    std::vector<Entry>::iterator find_it(OrderId id) {
        return std::find_if(entries_.begin(), entries_.end(),
                            [&](const Entry &e) { return e.order.orderId == id; });
    }
    std::vector<Entry>::const_iterator find_it(OrderId id) const {
        return std::find_if(entries_.begin(), entries_.end(),
                            [&](const Entry &e) { return e.order.orderId == id; });
    }

    // 选 side 盘里 价格最优（买取最高 / 卖取最低）且 seq 最小 的那笔。
    [[nodiscard]] std::optional<Order> best(Side side, bool max_price) const {
        const Entry *chosen = nullptr;
        for (const Entry &e : entries_) {
            if (e.order.side != side) continue;
            if (chosen == nullptr) {
                chosen = &e;
                continue;
            }
            const bool priceBetter = max_price ? (e.order.price > chosen->order.price)
                                               : (e.order.price < chosen->order.price);
            if (priceBetter || (e.order.price == chosen->order.price && e.seq < chosen->seq)) {
                chosen = &e;
            }
        }
        if (chosen == nullptr) return std::nullopt;
        return chosen->order;
    }
};

} // namespace test_support
} // namespace maelstrom
