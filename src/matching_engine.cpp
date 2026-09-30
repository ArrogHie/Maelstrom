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

    auto &order_book = book();

    if (order_book.find(order.orderId) != std::nullopt) {
        return {OrderOutcome::Rejected, RejectReason::DuplicateOrderId};
    }

    if (order.type != OrderType::Market && order.price.get() <= 0U) {
        return {OrderOutcome::Rejected, RejectReason::InvalidPrice};
    }

    Side needside = Side::Buy;

    std::function<std::optional<Order>()> best = [&order_book]() -> std::optional<Order> {
        return order_book.best_bid();
    };

    if (order.side == Side::Buy) {
        best = [&order_book]() -> std::optional<Order> {
            return order_book.best_ask();
        };
        needside = Side::Sell;
    }

    if (order.type == OrderType::PostOnly) {
        std::optional<Order> passive_order = best();
        if (order.side == Side::Buy) {
            if (passive_order.has_value() && passive_order.value().price <= order.price) {
                return {OrderOutcome::Rejected, RejectReason::PostOnlyWouldCross};
            }
        }
        else {
            if (passive_order.has_value() && passive_order.value().price >= order.price) {
                return {OrderOutcome::Rejected, RejectReason::PostOnlyWouldCross};
            }
        }
    }

    if (order.type == OrderType::Fok) {
        if (order_book.matchable_quantity(needside, order.price) < order.quantity) {
            return {OrderOutcome::Rejected, RejectReason::FokNotFillable};
        }
    }

    Quantity     remaining = order.quantity;
    unsigned int trade_cnt{0};

    while (remaining.get() > 0U) {
        if (order.type != OrderType::Market) {
            std::optional<Order> nowbest = best();
            if (!nowbest.has_value()) break;
            if (order.side == Side::Buy) {
                if (nowbest.value().price > order.price) break;
            }
            else {
                if (nowbest.value().price < order.price) break;
            }
        }

        std::optional<Fill> opt_fill = book_.consume_best(needside, remaining);
        if (!opt_fill.has_value()) break;

        Fill &fill = opt_fill.value();

        remaining = remaining - fill.quantity;
        Trade trade{order.orderId, fill.passive_id, fill.price, fill.quantity, order.side};
        if (handler_) {
            handler_(trade);
        }
        ++trade_cnt;
    }

    SubmitResult submit{OrderOutcome::Filled, RejectReason::None, order.quantity - remaining, Quantity{0}, trade_cnt};
    if (remaining.get() > 0U) {
        if (order.type == OrderType::Market || order.type == OrderType::Ioc) {
            submit.outcome = OrderOutcome::Cancelled;
        }
        else {
            if (remaining == order.quantity) {
                submit.outcome = OrderOutcome::Rested;
            }
            else {
                submit.outcome = OrderOutcome::PartiallyFilled;
            }
            order.quantity = remaining;
            submit.resting = remaining;
            book_.add(order);
        }
    }
    return submit;
}
}; // namespace matching_engine