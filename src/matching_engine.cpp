#include "Maelstrom/matching_engine.h"

#include "Maelstrom/order.h"
#include "Maelstrom/order_book.h"

using namespace maelstrom;

namespace matching_engine {

void MatchingEngine::set_trade_handler(TradeHandler trade) {
    handler_ = trade;
}

const OrderBook &MatchingEngine::book() const noexcept {
    return book_;
}

bool MatchingEngine::cancel(OrderId id) {
    return book_.cancel(id);
}

SubmitResult MatchingEngine::submit(Order order) {
    if (order.quantity.get() <= 0U) {
        return {OrderOutcome::Rejected, RejectReason::InvalidQuantity};
    }
    if (order.price.get() <= 0U) {
        return {OrderOutcome::Rejected, RejectReason::InvalidPrice};
    }

    auto &order_book = book();

    std::function<std::optional<Order>()> best = [&order_book]() -> std::optional<Order> {
        return order_book.best_ask();
    };

    if (order.side == Side::Buy) {
        best = [&order_book]() -> std::optional<Order> {
            return order_book.best_bid();
        };
    }

    if (order_book.find(order.orderId) != std::nullopt) {
        return {OrderOutcome::Rejected, RejectReason::DuplicateOrderId};
    }

    if (order.type == OrderType::PostOnly) {
        std::optional<Order> passive_order = best();
        if (passive_order.has_value() && passive_order.value().price <= order.price) {
            return {OrderOutcome::Rejected, RejectReason::PostOnlyWouldCross};
        }
    }

    if (order.type == OrderType::Fok) {
        if (order_book.matchable_quantity(order.side, order.price) < order.quantity) {
            return {OrderOutcome::Rejected, RejectReason::FokNotFillable};
        }
    }

    Quantity     remaining = order.quantity;
    unsigned int trade_cnt{0};
    while (remaining.get() > 0U) {
        std::optional<Fill> opt_fill = book_.consume_best(order.side, remaining);
        if (!opt_fill.has_value()) break;

        Fill &fill = opt_fill.value();

        remaining = remaining - fill.quantity;
        Trade trade{order.orderId, fill.passive_id, fill.price, fill.quantity, order.side};
        if (handler_) {
            handler_(trade);
        }
        ++trade_cnt;
    }

    SubmitResult submit{OrderOutcome::Filled, RejectReason::None, order.quantity - remaining, remaining, trade_cnt};
    if (remaining == order.quantity) {
        submit.outcome = OrderOutcome::Rested;
    }
    else if (remaining.get() > 0U) {
        submit.outcome = OrderOutcome::PartiallyFilled;
    }

    if (remaining.get() > 0U) {
        order.quantity = remaining;
        book_.add(order);
    }

    return submit;
}
}; // namespace matching_engine