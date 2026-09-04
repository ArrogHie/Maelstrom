#pragma once
#include <cstdint>

#include "Maelstrom/types.h"

namespace maelstrom {

enum class Side : std::int8_t {
    Sell,
    Buy
};

class Order {
public:
    Price    price;
    OrderId  orderId;
    Quantity quantity;
    Side     side;
};
} // namespace maelstrom