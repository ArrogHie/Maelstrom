#include "Maelstrom/order_book.h"

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Maelstrom/order.h"

namespace maelstrom {

OrderBook::OrderBook()                                 = default;
OrderBook::~OrderBook()                                = default;
OrderBook::OrderBook(OrderBook &&) noexcept            = default;
OrderBook &OrderBook::operator=(OrderBook &&) noexcept = default;

class OrderBook::OrderNode {
public:
    Order      order;
    OrderNode *pre = nullptr, *nxt = nullptr;
    Level     *level = nullptr;

    OrderNode() = delete;
    OrderNode(const Order &o) : order(o) {}
    OrderNode(const OrderNode &)                = delete;
    OrderNode &operator=(const OrderNode &)     = delete;
    OrderNode(OrderNode &&) noexcept            = default;
    OrderNode &operator=(OrderNode &&) noexcept = default;

    ~OrderNode();

    void modify(Quantity newQuant);
};

class OrderBook::Level {
public:
    OrderNode *head = nullptr, *tail = nullptr;
    Quantity   totQuant;
    Price      price;
    Side       side;
    size_t     size = 0;

    Level() = delete;
    Level(Price p, Side s) : price(p), side(s) {}
    void insert(OrderNode *p) {
        p->level = this;
        if (size == 0U) {
            head = p;
            tail = p;
        }
        else {
            tail->nxt = p;
            p->pre    = tail;
            tail      = p;
        }
        totQuant = totQuant + p->order.quantity;
        size++;
    }

    void remove(OrderNode *p) {
        OrderNode *&pre = p->pre;
        OrderNode *&nxt = p->nxt;
        pre != nullptr ? (pre->nxt = nxt) : (head = nxt);
        nxt != nullptr ? (nxt->pre = pre) : (tail = pre);
        pre = nxt = nullptr;
        p->level  = nullptr;
        totQuant  = totQuant - p->order.quantity;
        size--;
    }
};

OrderBook::OrderNode::~OrderNode() {
    if (level != nullptr) {
        level->remove(this);
    }
}

void OrderBook::OrderNode::modify(Quantity newQuant) {
    level->totQuant = level->totQuant - order.quantity + newQuant;
    order.quantity  = newQuant;
}

struct OrderBook::OrderIdHash {
    size_t operator()(const OrderId &order) const {
        return std::hash<std::int64_t>{}(order.get());
    }
};

struct OrderBook::Impl {
    std::map<Price, Level, std::greater<>>                               orders_bid;
    std::map<Price, Level>                                               orders_ask;
    std::unordered_map<OrderId, std::unique_ptr<OrderNode>, OrderIdHash> orders;

    ~Impl() { orders.clear(); }
};

bool OrderBook::check_order(const Order &o) const {
    if (o.orderId.get() <= 0) {
        return false;
    }
    if (o.price.get() <= 0) {
        return false;
    }
    if (o.quantity.get() <= 0) {
        return false;
    }
    return true;
}

bool OrderBook::add(Order o) {
    if (!check_order(o)) {
        return false;
    }
    if (!impl) {
        impl = std::make_unique<Impl>();
    }
    auto &orders = impl->orders;
    // 使用emplace可以只进行一次hash查询
    auto [it, inserted] = orders.emplace(o.orderId, std::make_unique<OrderNode>(o));
    if (!inserted) {
        return false;
    }

    OrderNode *p = it->second.get();
    if (o.side == Side::Buy) {
        auto &bid              = impl->orders_bid;
        auto [bid_it, bid_ins] = bid.emplace(o.price, Level(o.price, Side::Buy));
        bid_it->second.insert(p);
    }
    else {
        auto &ask              = impl->orders_ask;
        auto [ask_it, ask_ins] = ask.emplace(o.price, Level(o.price, Side::Sell));
        ask_it->second.insert(p);
    }
    return true;
}

bool OrderBook::cancel(OrderId id) {
    if (!impl) {
        return false;
    }
    auto &orders = impl->orders;
    auto  it     = orders.find(id);
    if (it == orders.end()) {
        return false; // 该订单不存在
    }

    OrderNode *p     = it->second.get();
    Level     *level = p->level;

    orders.erase(it);
    if (level->size == 0U) {
        auto price = level->price;
        auto side  = level->side;

        if (side == Side::Buy) {
            impl->orders_bid.erase(price);
        }
        else {
            impl->orders_ask.erase(price);
        }
    }
    return true;
}

bool OrderBook::modify(OrderId id, Quantity newQuant) {
    if (newQuant.get() <= 0) {
        return false;
    }
    if (!impl) {
        return false;
    }
    auto it = impl->orders.find(id);
    if (it == impl->orders.end()) {
        return false;
    }
    it->second->modify(newQuant);
    return true;
}

bool OrderBook::modify(OrderId id, Price p, Quantity newQuant) {
    if (p.get() <= 0 || newQuant.get() <= 0) {
        return false;
    }
    if (!impl) {
        return false;
    }
    auto it = impl->orders.find(id);
    if (it == impl->orders.end()) {
        return false;
    }
    if (it->second->order.price == p) {
        it->second->modify(newQuant);
        return true;
    }
    auto side = it->second->order.side;
    this->cancel(id);
    this->add(Order{p, id, newQuant, side});
    return true;
}

std::optional<Order> OrderBook::best_bid() const {
    if (!impl || impl->orders_bid.empty()) {
        return std::nullopt;
    }
    auto it = impl->orders_bid.begin();
    return it->second.head->order;
}

std::optional<Order> OrderBook::best_ask() const {
    if (!impl || impl->orders_ask.empty()) {
        return std::nullopt;
    }
    auto it = impl->orders_ask.begin();
    return it->second.head->order;
}

std::size_t OrderBook::size() const {
    if (!impl) {
        return 0;
    }
    return impl->orders.size();
}

std::optional<Order> OrderBook::find(OrderId id) const {
    if (!impl) {
        return std::nullopt;
    }
    auto it = impl->orders.find(id);
    if (it == impl->orders.end()) {
        return std::nullopt;
    }
    return it->second->order;
}

Quantity OrderBook::quantity_at(Price p) const {
    if (!impl) {
        return Quantity{0};
    }
    auto it_bid = impl->orders_bid.find(p);
    auto it_ask = impl->orders_ask.find(p);

    Quantity res{0};
    if (it_bid != impl->orders_bid.end()) {
        res = res + it_bid->second.totQuant;
    }
    if (it_ask != impl->orders_ask.end()) {
        res = res + it_ask->second.totQuant;
    }
    return res;
}

std::optional<Fill> OrderBook::consume_best(Side side, Quantity qty) {
    if (!impl) {
        return std::nullopt;
    }
    std::optional<Order> opt_order;
    if (side == Side::Buy) {
        opt_order = this->best_bid();
    }
    else {
        opt_order = this->best_ask();
    }
    if (!opt_order.has_value()) {
        return std::nullopt;
    }

    Order order = opt_order.value();
    if (qty >= order.quantity) {
        this->cancel(order.orderId);
        return Fill{0, order.price, order.quantity};
    }
    else {
        this->modify(order.orderId, order.quantity - qty);
        return Fill{0, order.price, qty};
    }
}

std::optional<Quantity> OrderBook::matchalbe_quantity(Side side, std::optional<Price> limit) const {
    if (!impl) {
        return std::nullopt;
    }
    Quantity tot{0};
    if (side == Side::Buy) {
        auto &orders = impl->orders_ask;
        for (auto &it : orders) {
            if (limit.has_value() && it.second.price > limit.value()) break;
            tot = tot + it.second.totQuant;
        }
    }
    else {
        auto &orders = impl->orders_bid;
        for (auto &it : orders) {
            if (limit.has_value() && it.second.price > limit.value()) break;
            tot = tot + it.second.totQuant;
        }
    }
    return tot;
}

} // namespace maelstrom