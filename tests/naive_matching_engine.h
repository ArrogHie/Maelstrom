// AI 提供的隔离验收代码（仅供测试差分对拍，不参与生产库）。
//
// NaiveMatchingEngine —— 语义与 docs/W3_MATCHING_GUIDE.md 判据逐条一致的
// “慢速参考撮合引擎”(oracle)。实现刻意直白（vector + 全扫，O(n)），只求
// “简单到不容易写错”：随机喂相同 `submit` 序列，逐步比对
//   - SubmitResult（outcome / reason / filled / resting / trade_count）
//   - 本次产生的成交序列（id / price / qty / side，按顺序逐一比对）
//   - 撮合后的簿状态（size / best_bid / best_ask / find / quantity_at）
//
// 语义契约（与 W3 判据一致）：
//  - 校验顺序：quantity<=0 -> InvalidQuantity；id 已存在 -> DuplicateOrderId；
//              非市价 price<=0 -> InvalidPrice。
//  - PostOnly 会立即成交则 -> PostOnlyWouldCross（无副作用）。
//  - FOK 可成交量 < 下单量 -> FokNotFillable（无副作用）。
//  - 撮合按 price-time：价格优先、同价 seq 小者优先；成交价 = 被动方价格。
//  - 余量：Limit/PostOnly 挂簿；Market/IOC 取消；FOK 必全额成交。
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "Maelstrom/matching_engine.h"
#include "Maelstrom/order.h"

using namespace matching_engine;

namespace maelstrom {
namespace test_support {

class NaiveMatchingEngine {
public:
    NaiveMatchingEngine() = default;

    SubmitResult submit(Order o) {
        last_trades_.clear();
        if (o.quantity.get() <= 0) return rejected(RejectReason::InvalidQuantity);
        if (index_of(o.orderId) >= 0) return rejected(RejectReason::DuplicateOrderId);

        const bool is_market = (o.type == OrderType::Market);
        if (!is_market && o.price.get() <= 0) return rejected(RejectReason::InvalidPrice);

        const Side opp = (o.side == Side::Buy) ? Side::Sell : Side::Buy;

        if (o.type == OrderType::PostOnly) {
            const int best = best_index(opp, std::nullopt, o.side);
            if (best >= 0 &&
                crosses(o.side, o.price, rest_[static_cast<std::size_t>(best)].order.price)) {
                return rejected(RejectReason::PostOnlyWouldCross);
            }
        }

        if (o.type == OrderType::Fok) {
            std::int64_t avail = 0;
            for (const Entry &e : rest_) {
                if (e.order.side != opp) continue;
                if (is_market || crosses(o.side, o.price, e.order.price)) {
                    avail += e.order.quantity.get();
                }
            }
            if (avail < o.quantity.get()) return rejected(RejectReason::FokNotFillable);
        }

        std::int64_t remaining = o.quantity.get();
        std::int64_t filled    = 0;
        std::size_t  trades    = 0;

        while (remaining > 0) {
            const int bi = best_index(opp, is_market ? std::nullopt : std::optional<Price>{o.price},
                                      o.side);
            if (bi < 0) break;
            Entry             &passive = rest_[static_cast<std::size_t>(bi)];
            const std::int64_t take =
                std::min(remaining, passive.order.quantity.get());
            last_trades_.push_back(
                Trade{o.orderId, passive.order.orderId, passive.order.price, Quantity{take}, o.side});
            filled += take;
            remaining -= take;
            ++trades;
            if (take >= passive.order.quantity.get()) {
                rest_.erase(rest_.begin() + bi);
            }
            else {
                passive.order.quantity = Quantity{passive.order.quantity.get() - take};
            }
        }

        Quantity   rest{0};
        const bool can_rest = (o.type == OrderType::Limit || o.type == OrderType::PostOnly);
        if (remaining > 0 && can_rest) {
            Order r    = o;
            r.quantity = Quantity{remaining};
            rest_.push_back(Entry{r, next_seq_++});
            rest = Quantity{remaining};
        }

        SubmitResult res;
        res.filled      = Quantity{filled};
        res.resting     = rest;
        res.trade_count = trades;
        if (rest.get() > 0) {
            res.outcome = (filled > 0) ? OrderOutcome::PartiallyFilled : OrderOutcome::Rested;
        }
        else if (filled == o.quantity.get()) {
            res.outcome = OrderOutcome::Filled; // 全额成交
        }
        else {
            res.outcome = OrderOutcome::Cancelled; // 有残余但被取消，未挂簿
        }
        return res;
    }

    [[nodiscard]] const std::vector<Trade> &last_trades() const { return last_trades_; }

    bool cancel(OrderId id) {
        const int i = index_of(id);
        if (i < 0) return false;
        rest_.erase(rest_.begin() + i);
        return true;
    }

    [[nodiscard]] std::optional<Order> best_bid() const { return best_of(Side::Buy); }
    [[nodiscard]] std::optional<Order> best_ask() const { return best_of(Side::Sell); }

    [[nodiscard]] std::optional<Order> find(OrderId id) const {
        const int i = index_of(id);
        if (i < 0) return std::nullopt;
        return rest_[static_cast<std::size_t>(i)].order;
    }

    [[nodiscard]] Quantity quantity_at(Price p) const {
        std::int64_t sum = 0;
        for (const Entry &e : rest_) {
            if (e.order.price == p) sum += e.order.quantity.get();
        }
        return Quantity{sum};
    }

    [[nodiscard]] std::size_t size() const { return rest_.size(); }

private:
    struct Entry {
        Order         order;
        std::uint64_t seq = 0;
    };

    static SubmitResult rejected(RejectReason r) {
        SubmitResult res;
        res.outcome = OrderOutcome::Rejected;
        res.reason  = r;
        return res;
    }

    // 主动方视角：被动价 passive 是否与限价 limit 交叉。
    static bool crosses(Side aggressor, Price limit, Price passive) {
        return aggressor == Side::Buy ? (passive.get() <= limit.get())
                                      : (passive.get() >= limit.get());
    }

    [[nodiscard]] int index_of(OrderId id) const {
        for (std::size_t i = 0; i < rest_.size(); ++i) {
            if (rest_[i].order.orderId == id) return static_cast<int>(i);
        }
        return -1;
    }

    // side 盘里 价格最优 + seq 最小 的下标；limit 有值时只考虑交叉的价位。
    [[nodiscard]] int best_index(Side side, std::optional<Price> limit, Side aggressor) const {
        int best = -1;
        for (std::size_t i = 0; i < rest_.size(); ++i) {
            const Entry &e = rest_[i];
            if (e.order.side != side) continue;
            if (limit.has_value() && !crosses(aggressor, *limit, e.order.price)) continue;
            if (best < 0) {
                best = static_cast<int>(i);
                continue;
            }
            const Entry &b      = rest_[static_cast<std::size_t>(best)];
            const bool   better = (side == Side::Sell) ? (e.order.price < b.order.price)
                                                       : (e.order.price > b.order.price);
            if (better || (e.order.price == b.order.price && e.seq < b.seq)) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    [[nodiscard]] std::optional<Order> best_of(Side side) const {
        const int i = best_index(side, std::nullopt, side == Side::Buy ? Side::Sell : Side::Buy);
        if (i < 0) return std::nullopt;
        return rest_[static_cast<std::size_t>(i)].order;
    }

    std::vector<Entry> rest_;
    std::uint64_t      next_seq_ = 1;
    std::vector<Trade> last_trades_;
};

} // namespace test_support
} // namespace maelstrom
