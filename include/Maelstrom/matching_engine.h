#pragma once
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

    bool accepted() { return outcome != OrderOutcome::Rejected; }
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

    bool cancel(OrderId id);

    [[nodiscard]] SubmitResult submit(Order order);

    const OrderBook &book() const noexcept;

private:
    OrderBook    book_;
    TradeHandler handler_;
};
} // namespace matching_engine
