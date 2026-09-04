#include "Maelstrom/order_book.h"

#include <algorithm>
#include <vector>

#include "Maelstrom/order.h"

namespace maelstrom {

struct OrderBook::Impl {
    std::vector<Order> orders;
};

void OrderBook::add(Order o) {
    if (!impl) impl = new Impl;
    impl->orders.push_back(o);
}

bool OrderBook::cancel(OrderId id) {
    if (!impl) return false;
    auto &v  = impl->orders;
    auto  it = std::find_if(v.begin(), v.end(), [&id](Order &o) { return o.orderId.get() == id.get(); });
    if (it == v.end()) return false;
    v.erase(it);
    return true;
}

std::optional<Order> OrderBook::best_bid() const {
    if (!impl || impl->orders.empty()) return std::nullopt;
    auto &v  = impl->orders;
    auto  it = std::max_element(v.begin(), v.end(), [](const Order &a, const Order &b) { return a.price.get() < b.price.get(); });
    return *it;
}

std::optional<Order> OrderBook::best_ask() const {
    if (!impl || impl->orders.empty()) return std::nullopt;
    auto &v  = impl->orders;
    auto  it = std::min_element(v.begin(), v.end(), [](const Order &a, const Order &b) { return a.price.get() < b.price.get(); });
    return *it;
}

std::size_t OrderBook::size() const {
    if (!impl) return 0;
    return impl->orders.size();
}

} // namespace maelstrom