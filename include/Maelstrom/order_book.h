#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

#include "Maelstrom/order.h"

namespace maelstrom {

class OrderBook {
public:
    void add(Order o);
    bool cancel(OrderId id);

    [[nodiscard]] std::optional<Order> best_bid() const; // 查询最高价
    [[nodiscard]] std::optional<Order> best_ask() const; // 查询最低价
    [[nodiscard]] std::size_t          size() const;

private:
    struct Impl; // 定义写在外部文件，保持接口稳定
    Impl *impl = nullptr;
};

} // namespace maelstrom