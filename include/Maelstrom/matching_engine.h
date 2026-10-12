#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>

#include "order.h"
#include "order_book.h"

using namespace maelstrom;

namespace matching_engine {
enum class OrderOutcome : std::int8_t {
    Filled,
    PartiallyFilled,
    Rested,
    Cancelled,
    Rejected
};

enum class RejectReason : std::int8_t {
    None,
    InvalidQuantity,
    DuplicateOrderId,
    InvalidPrice,
    PostOnlyWouldCross,
    FokNotFillable
};

enum class EventKind : std::int8_t {
    Trade,         // 成交
    OrderRested,   // 挂薄
    OrderCanceled, // 撤单/余量取消
    OrderRejected, // 拒单
};

enum class SelfMatchPolicy : std::int8_t {
    CancelNewset,
    CancelOldset,
};

struct Trade {
    OrderId  aggressive_id;
    OrderId  passive_id;
    Price    price;
    Quantity quantity;
    Side     aggressor_side;
};

struct SubmitResult {
    OrderOutcome outcome;
    RejectReason reason = RejectReason::None;
    Quantity     filled{0};
    Quantity     resting{0};
    unsigned int trade_count{0};

    bool accepted() const { return outcome != OrderOutcome::Rejected; }
};

struct Event {
    EventKind    kind;
    OrderId      order_id{0};
    AccountId    account{0};
    Side         side{Side::Sell};
    OrderId      couterparty_id{0}; // 对手方id
    Price        price{0};
    Quantity     quantity{0};
    RejectReason reason{RejectReason::None};
};

class EventBus { // 事件总线
public:
    using Handler = std::function<void(const Event &)>;

    SubscriptionId subscribe(Handler handler) {
        slots_.emplace_back(Slot{next_id, std::move(handler)});
        next_id = next_id + SubscriptionId{1};
        return next_id - SubscriptionId{1};
    }

    bool unsubscribe(SubscriptionId id) {
        for (auto slot = slots_.begin(); slot != slots_.end(); slot++) {
            if (slot->id == id) {
                slots_.erase(slot);
                return true;
            }
        }
        return false;
    }

    void publish(const Event &event) {
        for (auto &slot : slots_) {
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
    SubscriptionId    next_id{1};
};

class MatchingEngine {
public:
    MatchingEngine() = default;

    MatchingEngine(const MatchingEngine &)            = delete;
    MatchingEngine &operator=(const MatchingEngine &) = delete;
    MatchingEngine(MatchingEngine &&)                 = default;
    MatchingEngine &operator=(MatchingEngine &&)      = default;

    using TradeHandler = std::function<void(const Trade &)>;

    void set_trade_handler(TradeHandler trade);
    void set_self_match_policy(SelfMatchPolicy policy);
    bool cancel(OrderId id);

    bool unsubscribe(SubscriptionId id);
    void publish(const Event &event);

    [[nodiscard]] SubscriptionId subscribe(EventBus::Handler handler);

    [[nodiscard]] SubmitResult     submit(Order order);
    [[nodiscard]] const OrderBook &book() const noexcept;

private:
    OrderBook       book_;
    TradeHandler    handler_;
    EventBus        bus_;
    SelfMatchPolicy policy_{SelfMatchPolicy::CancelNewset};
};
} // namespace matching_engine
