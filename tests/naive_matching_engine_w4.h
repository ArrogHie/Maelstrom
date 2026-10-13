// AI 提供的隔离验收代码（仅供测试差分对拍，不参与生产库）。
//
// NaiveMatchingEngineW4 —— W4 语义（账户 + 自成交防护 + 事件流）的慢速参考引擎
// (oracle)，契约与 docs/W4_MATCHING_GUIDE.md 的"判据"逐条一致。在 W3 oracle 的
// 直白风格（vector + 全扫）上增加：
//   - Order.account 与自成交防护（CancelNewest / CancelOldest，执行接触点判定）
//   - FOK 的 STP 感知全额成交预检（在副本上干跑：CN 撞上自己即失败，
//     CO 撤掉自己继续；失败 = 零副作用；FOK 永不部分成交、永不挂簿）
//   - 事件流记录（events()：Trade / OrderRested / OrderCanceled / OrderRejected）
// 差分测试喂相同 submit/cancel 序列，逐步比对 SubmitResult / 事件流 / 簿状态。
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

class NaiveMatchingEngineW4 {
public:
    NaiveMatchingEngineW4() = default;

    void set_policy(SelfMatchPolicy policy) { policy_ = policy; }

    SubmitResult submit(Order o) {
        events_.clear();
        if (o.quantity.get() <= 0) return rejected(o, RejectReason::InvalidQuantity);
        if (index_of(o.orderId) >= 0) return rejected(o, RejectReason::DuplicateOrderId);

        const bool is_market = (o.type == OrderType::Market);
        if (!is_market && o.price.get() <= 0) return rejected(o, RejectReason::InvalidPrice);

        const Side opp = (o.side == Side::Buy) ? Side::Sell : Side::Buy;

        // POST-ONLY 预检只看价格，不看账户（与判据一致：拒单是安全方向）。
        if (o.type == OrderType::PostOnly) {
            const int best = best_index(opp, std::nullopt, o.side);
            if (best >= 0 &&
                crosses(o.side, o.price, rest_[static_cast<std::size_t>(best)].order.price)) {
                return rejected(o, RejectReason::PostOnlyWouldCross);
            }
        }

        // FOK 预检：STP 感知的"可达流动性"——在副本上干跑一次撮合（纯读、零副作用）。
        // CN：撞上自己的挂单即失败（余量会被 STP 砍掉，无法全额成交）；
        // CO：撤掉自己的挂单继续；失败时真实簿不得有任何变化。
        if (o.type == OrderType::Fok) {
            std::vector<Entry>         sim = rest_;
            const std::optional<Price> flimit =
                is_market ? std::nullopt : std::optional<Price>{o.price};
            std::int64_t remaining = o.quantity.get();
            bool         fillable  = true;
            while (remaining > 0) {
                const int bi = best_index_in(sim, opp, flimit, o.side);
                if (bi < 0) {
                    fillable = false;
                    break;
                }
                const std::size_t si = static_cast<std::size_t>(bi);
                if (o.account.get() != 0 && sim[si].order.account == o.account) {
                    if (policy_ == SelfMatchPolicy::CancelNewest) {
                        fillable = false;
                        break;
                    }
                    sim.erase(sim.begin() + bi);
                    continue;
                }
                const std::int64_t take = std::min(remaining, sim[si].order.quantity.get());
                remaining -= take;
                if (take >= sim[si].order.quantity.get()) {
                    sim.erase(sim.begin() + bi);
                }
                else {
                    sim[si].order.quantity = Quantity{sim[si].order.quantity.get() - take};
                }
            }
            if (!fillable) return rejected(o, RejectReason::FokNotFillable);
        }

        std::int64_t remaining  = o.quantity.get();
        std::int64_t filled     = 0;
        std::size_t  trades     = 0;
        bool         self_killed = false;

        while (remaining > 0) {
            const int bi = best_index(opp, is_market ? std::nullopt
                                                     : std::optional<Price>{o.price},
                                      o.side);
            if (bi < 0) break;
            Entry &passive = rest_[static_cast<std::size_t>(bi)];

            // 自成交防护：执行接触点（只看队头这一笔）；账户相同且非 0 才触发。
            if (o.account.get() != 0 && o.account == passive.order.account) {
                if (policy_ == SelfMatchPolicy::CancelNewest) {
                    push_canceled(o.orderId, o.account, o.side, Quantity{remaining});
                    self_killed = true;
                    break;
                }
                push_canceled(passive.order.orderId, passive.order.account, passive.order.side,
                              passive.order.quantity);
                rest_.erase(rest_.begin() + bi);
                continue;
            }

            const std::int64_t take = std::min(remaining, passive.order.quantity.get());
            push_trade(o, passive.order, Quantity{take});
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
        const bool can_rest =
            !self_killed && (o.type == OrderType::Limit || o.type == OrderType::PostOnly);
        if (remaining > 0 && can_rest) {
            Order r    = o;
            r.quantity = Quantity{remaining};
            rest_.push_back(Entry{r, next_seq_++});
            rest = Quantity{remaining};
            Event ev{};
            ev.kind     = EventKind::OrderRested;
            ev.order_id = r.orderId;
            ev.account  = r.account;
            ev.side     = r.side;
            ev.price    = r.price;
            ev.quantity = rest;
            events_.push_back(ev);
        }
        else if (remaining > 0 && !self_killed) {
            push_canceled(o.orderId, o.account, o.side, Quantity{remaining});
        }

        SubmitResult res;
        res.filled      = Quantity{filled};
        res.resting     = rest;
        res.trade_count = trades;
        if (rest.get() > 0) {
            res.outcome = (filled > 0) ? OrderOutcome::PartiallyFilled : OrderOutcome::Rested;
        }
        else if (filled == o.quantity.get()) {
            res.outcome = OrderOutcome::Filled;
        }
        else {
            res.outcome = OrderOutcome::Cancelled;
        }
        return res;
    }

    bool cancel(OrderId id) {
        const int i = index_of(id);
        if (i < 0) return false;
        const Entry &e = rest_[static_cast<std::size_t>(i)];
        push_canceled(e.order.orderId, e.order.account, e.order.side, e.order.quantity);
        rest_.erase(rest_.begin() + i);
        return true;
    }

    [[nodiscard]] const std::vector<Event> &events() const { return events_; }

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

    SubmitResult rejected(const Order &o, RejectReason r) {
        Event ev{};
        ev.kind     = EventKind::OrderRejected;
        ev.order_id = o.orderId;
        ev.account  = o.account;
        ev.side     = o.side;
        ev.reason   = r;
        events_.push_back(ev);
        SubmitResult res;
        res.outcome = OrderOutcome::Rejected;
        res.reason  = r;
        return res;
    }

    static void fill_event(Event &ev, EventKind kind, OrderId id, AccountId account, Side side) {
        ev.kind     = kind;
        ev.order_id = id;
        ev.account  = account;
        ev.side     = side;
    }

    void push_trade(const Order &aggressor, const Order &passive, Quantity qty) {
        Event ev{};
        fill_event(ev, EventKind::Trade, aggressor.orderId, aggressor.account, aggressor.side);
        ev.counterparty_id = passive.orderId;
        ev.price           = passive.price; // 被动价
        ev.quantity        = qty;
        events_.push_back(ev);
    }

    void push_canceled(OrderId id, AccountId account, Side side, Quantity qty) {
        Event ev{};
        fill_event(ev, EventKind::OrderCanceled, id, account, side);
        ev.quantity = qty;
        events_.push_back(ev);
    }

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

    // 在任意挂单集合 v 上找 价格最优 + seq 最小 的下标（供 FOK 预检的副本干跑复用）。
    static int best_index_in(const std::vector<Entry> &v, Side side, std::optional<Price> limit,
                             Side aggressor) {
        int best = -1;
        for (std::size_t i = 0; i < v.size(); ++i) {
            const Entry &e = v[i];
            if (e.order.side != side) continue;
            if (limit.has_value() && !crosses(aggressor, *limit, e.order.price)) continue;
            if (best < 0) {
                best = static_cast<int>(i);
                continue;
            }
            const Entry &b      = v[static_cast<std::size_t>(best)];
            const bool   better = (side == Side::Sell) ? (e.order.price < b.order.price)
                                                       : (e.order.price > b.order.price);
            if (better || (e.order.price == b.order.price && e.seq < b.seq)) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    [[nodiscard]] int best_index(Side side, std::optional<Price> limit, Side aggressor) const {
        return best_index_in(rest_, side, limit, aggressor);
    }

    [[nodiscard]] std::optional<Order> best_of(Side side) const {
        const int i = best_index(side, std::nullopt, side == Side::Buy ? Side::Sell : Side::Buy);
        if (i < 0) return std::nullopt;
        return rest_[static_cast<std::size_t>(i)].order;
    }

    std::vector<Entry>    rest_;
    std::uint64_t         next_seq_ = 1;
    std::vector<Event>    events_;
    SelfMatchPolicy       policy_ = SelfMatchPolicy::CancelNewest;
};

} // namespace test_support
} // namespace maelstrom
