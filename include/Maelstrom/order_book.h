#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "Maelstrom/order.h"

namespace maelstrom {

struct Fill {
    OrderId  passive_id;
    Price    price;
    Quantity quantity;
};

class OrderBook {
public:
    OrderBook();
    ~OrderBook();

    OrderBook(const OrderBook &)            = delete; // 禁止拷贝
    OrderBook &operator=(const OrderBook &) = delete;
    OrderBook(OrderBook &&) noexcept;
    OrderBook &operator=(OrderBook &&) noexcept;

    bool add(Order o);
    bool cancel(OrderId id);
    bool modify(OrderId id, Quantity newQuant);          // 改量
    bool modify(OrderId id, Price p, Quantity newQuant); // 改价改量

    [[nodiscard]] std::size_t          size() const;
    [[nodiscard]] Quantity             quantity_at(Price p) const;
    [[nodiscard]] std::optional<Order> best_bid() const; // 买方报价，最高价优先
    [[nodiscard]] std::optional<Order> best_ask() const; // 卖方报价，最低价优先
    [[nodiscard]] std::optional<Order> find(OrderId id) const;
    [[nodiscard]] std::optional<Fill>  consume_best(Side side, Quantity qty);                           // 进行成交
    [[nodiscard]] Quantity             matchable_quantity(Side side, std::optional<Price> limit) const; // 可成交量

private:
    class Level;
    class OrderNode;

    struct Impl; // 定义写在外部文件，保持接口稳定
    struct OrderIdHash;
    std::unique_ptr<Impl> impl;

    bool check_order(const Order &o) const;
};

} // namespace maelstrom