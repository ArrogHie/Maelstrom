#pragma once
#include <cstdint>

#include "Maelstrom/types.h"

namespace maelstrom {

enum class Side : std::int8_t {
    Sell,
    Buy
};

enum class OrderType : std::int8_t {
    Limit,
    Market,
    Ioc,
    Fok,
    PostOnly
};

class Order {
public:
    Price     price;
    OrderId   orderId;
    Quantity  quantity;
    Side      side;
    OrderType type = OrderType::Limit;
};
} // namespace maelstrom