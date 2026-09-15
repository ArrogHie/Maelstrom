// W2 验收测试（AI 隔离编写，只作为正确性裁判）。
//
// 内容分两部分：
//   1. OrderBookW2.*        确定性单元测试：FIFO / 跨价排序 / 撤单 / 改量 /
//                           改价(重排队) / 非法参数 / id 复用 / move 语义等。
//   2. Differential/*       随机差分对拍：把你的 OrderBook 和慢速参考簿
//                           NaiveOrderBook 喂相同随机操作序列，逐步比对
//                           size / best_bid / best_ask / find / quantity_at。
//
// 语义契约见 docs/W2_ORDER_BOOK_GUIDE.md 的 A2 节，两者必须逐字一致。
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "Maelstrom/order_book.h"
#include "naive_order_book.h"

using namespace maelstrom;
using maelstrom::test_support::NaiveOrderBook;

namespace {

OrderId OID(std::int64_t v) { return OrderId{v}; }

// Order 字段顺序：{Price, OrderId, Quantity, Side}
Order mk(std::int64_t id, std::int64_t price, std::int64_t qty, Side side) {
    return Order{Price{price}, OID(id), Quantity{qty}, side};
}

void expect_order(const std::optional<Order> &got, std::int64_t id, std::int64_t price,
                  std::int64_t qty, Side side) {
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->orderId, OID(id));
    EXPECT_EQ(got->price.get(), price);
    EXPECT_EQ(got->quantity.get(), qty);
    EXPECT_EQ(got->side, side);
}

} // namespace

// ---------------------------------------------------------------------------
// 确定性单元测试
// ---------------------------------------------------------------------------

TEST(OrderBookW2, MoveSemanticsAndCopyDisabled) {
    static_assert(!std::is_copy_constructible<OrderBook>::value, "OrderBook must not be copyable");
    static_assert(!std::is_copy_assignable<OrderBook>::value, "OrderBook must not be copyable");
    static_assert(std::is_move_constructible<OrderBook>::value, "OrderBook must be movable");
    static_assert(std::is_move_assignable<OrderBook>::value, "OrderBook must be movable");

    OrderBook b1;
    ASSERT_TRUE(b1.add(mk(1, 100, 10, Side::Buy)));
    OrderBook b2(std::move(b1));
    expect_order(b2.best_bid(), 1, 100, 10, Side::Buy);

    OrderBook b3;
    b3 = std::move(b2);
    EXPECT_EQ(b3.size(), 1u);
    expect_order(b3.best_bid(), 1, 100, 10, Side::Buy);
}

TEST(OrderBookW2, EmptyBookQueries) {
    OrderBook book;
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.find(OID(1)).has_value());
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 0);
}

TEST(OrderBookW2, AddKeepsArrivalOrderAtSamePrice) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 100, 7, Side::Buy)));
    ASSERT_TRUE(book.add(mk(3, 100, 3, Side::Buy)));
    EXPECT_EQ(book.size(), 3u);
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy); // 最先到达的排最前
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 15);
}

TEST(OrderBookW2, BestBidPicksHighestPrice) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 1, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 103, 1, Side::Buy))); // 后到但价高 -> 赢
    ASSERT_TRUE(book.add(mk(3, 102, 1, Side::Buy)));
    expect_order(book.best_bid(), 2, 103, 1, Side::Buy);
    ASSERT_TRUE(book.add(mk(4, 105, 1, Side::Buy)));
    expect_order(book.best_bid(), 4, 105, 1, Side::Buy);
    ASSERT_TRUE(book.cancel(OID(4)));
    expect_order(book.best_bid(), 2, 103, 1, Side::Buy);
}

TEST(OrderBookW2, BestAskPicksLowestPrice) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 102, 1, Side::Sell)));
    ASSERT_TRUE(book.add(mk(2, 99, 1, Side::Sell)));
    ASSERT_TRUE(book.add(mk(3, 101, 1, Side::Sell)));
    expect_order(book.best_ask(), 2, 99, 1, Side::Sell);
    ASSERT_TRUE(book.cancel(OID(2)));
    expect_order(book.best_ask(), 3, 101, 1, Side::Sell);
}

TEST(OrderBookW2, BestIsSideIsolated) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());

    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy);
    EXPECT_FALSE(book.best_ask().has_value()); // 只有买盘时卖一无

    ASSERT_TRUE(book.add(mk(2, 101, 3, Side::Sell)));
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy);
    expect_order(book.best_ask(), 2, 101, 3, Side::Sell);
}

TEST(OrderBookW2, CancelTopRevealsNextInLine) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 100, 7, Side::Buy)));
    ASSERT_TRUE(book.cancel(OID(1)));
    expect_order(book.best_bid(), 2, 100, 7, Side::Buy);
    EXPECT_EQ(book.size(), 1u);
    ASSERT_TRUE(book.cancel(OID(2)));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBookW2, CancelInteriorPreservesRestOfQueue) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy))); // A
    ASSERT_TRUE(book.add(mk(2, 100, 7, Side::Buy))); // B
    ASSERT_TRUE(book.add(mk(3, 100, 3, Side::Buy))); // C
    ASSERT_TRUE(book.cancel(OID(2)));                // 撤中间，跳过 B
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy);
    ASSERT_TRUE(book.cancel(OID(1)));
    expect_order(book.best_bid(), 3, 100, 3, Side::Buy); // B 已被撤，不得复活
}

TEST(OrderBookW2, CancelUnknownAndTwice) {
    OrderBook book;
    EXPECT_FALSE(book.cancel(OID(9)));
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    EXPECT_TRUE(book.cancel(OID(1)));
    EXPECT_FALSE(book.cancel(OID(1)));
}

TEST(OrderBookW2, AddDuplicateRejectedAndIdCanBeReused) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    EXPECT_FALSE(book.add(mk(1, 101, 7, Side::Buy))); // 重复 id -> 拒绝
    EXPECT_EQ(book.size(), 1u);
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy); // 原单不受影响

    ASSERT_TRUE(book.cancel(OID(1)));
    ASSERT_TRUE(book.add(mk(1, 99, 4, Side::Buy))); // 撤后可复用 id
    expect_order(book.best_bid(), 1, 99, 4, Side::Buy);
}

TEST(OrderBookW2, AddRejectedOnInvalidArgs) {
    OrderBook book;
    EXPECT_FALSE(book.add(mk(1, 0, 10, Side::Buy)));
    EXPECT_FALSE(book.add(mk(2, -5, 10, Side::Buy)));
    EXPECT_FALSE(book.add(mk(3, 100, 0, Side::Buy)));
    EXPECT_FALSE(book.add(mk(4, 100, -3, Side::Sell)));
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBookW2, ModifyQuantityKeepsTimePriority) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 10, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 100, 7, Side::Buy)));

    EXPECT_FALSE(book.modify(OID(9), Quantity{5})); // id 不存在
    EXPECT_FALSE(book.modify(OID(1), Quantity{0})); // 非法数量
    EXPECT_FALSE(book.modify(OID(1), Quantity{-2}));

    EXPECT_TRUE(book.modify(OID(1), Quantity{3}));
    auto a = book.find(OID(1));
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->quantity.get(), 3); // 数量已改
    EXPECT_EQ(a->price.get(), 100);
    expect_order(book.best_bid(), 1, 100, 3, Side::Buy); // 仍在前，优先级保留
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 10);   // 3 + 7

    ASSERT_TRUE(book.cancel(OID(1)));
    expect_order(book.best_bid(), 2, 100, 7, Side::Buy);
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 7);
}

TEST(OrderBookW2, ModifyPriceRequeuesToTailAndLosesPriority) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy))); // A
    ASSERT_TRUE(book.add(mk(2, 100, 7, Side::Buy))); // B
    ASSERT_TRUE(book.add(mk(3, 100, 4, Side::Buy))); // C

    // A 改到 99：离开 100 档
    EXPECT_TRUE(book.modify(OID(1), Price{99}, Quantity{5}));
    auto a = book.find(OID(1));
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->price.get(), 99);
    expect_order(book.best_bid(), 2, 100, 7, Side::Buy); // 队头换成 B
    EXPECT_EQ(book.quantity_at(Price{99}).get(), 5);
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 11); // 7 + 4

    // A 改回 100：作为新单排到 B、C 之后
    EXPECT_TRUE(book.modify(OID(1), Price{100}, Quantity{5}));
    expect_order(book.best_bid(), 2, 100, 7, Side::Buy); // A 已失去原优先级
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 16);   // 7 + 4 + 5

    ASSERT_TRUE(book.cancel(OID(2)));
    expect_order(book.best_bid(), 3, 100, 4, Side::Buy);
    ASSERT_TRUE(book.cancel(OID(3)));
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy); // A 终于排到队头

    EXPECT_FALSE(book.modify(OID(99), Price{101}, Quantity{1})); // id 不存在
}

TEST(OrderBookW2, ModifyPriceSameAsOriginalKeepsPriority) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 100, 7, Side::Buy)));
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy);
    EXPECT_TRUE(book.modify(OID(1), Price{100}, Quantity{3}));
    expect_order(book.best_bid(), 1, 100, 3, Side::Buy); // 价格没变 -> 仍是队头
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 10);
}

TEST(OrderBookW2, ModifyPriceInvalidArgsRejected) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    EXPECT_FALSE(book.modify(OID(1), Price{0}, Quantity{5}));
    EXPECT_FALSE(book.modify(OID(1), Price{-1}, Quantity{5}));
    EXPECT_FALSE(book.modify(OID(1), Price{101}, Quantity{0}));
    EXPECT_FALSE(book.modify(OID(1), Price{101}, Quantity{-3}));
    auto a = book.find(OID(1));
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->price.get(), 100); // 原样未被破坏
    EXPECT_EQ(a->quantity.get(), 5);
    EXPECT_EQ(book.size(), 1u);
}

TEST(OrderBookW2, ModifyPriceMovesBetweenLevels) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 101, 7, Side::Buy)));
    expect_order(book.best_bid(), 2, 101, 7, Side::Buy);

    ASSERT_TRUE(book.modify(OID(2), Price{99}, Quantity{7})); // 撤出 101 档
    expect_order(book.best_bid(), 1, 100, 5, Side::Buy);
    EXPECT_EQ(book.quantity_at(Price{101}).get(), 0); // 空档被清掉
    EXPECT_EQ(book.quantity_at(Price{99}).get(), 7);
    auto b = book.find(OID(2));
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->price.get(), 99);
    EXPECT_EQ(b->side, Side::Buy);
}

TEST(OrderBookW2, QuantityAtAggregatesBothSidesAtSamePrice) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 4, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 100, 3, Side::Sell)));
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 7); // 买 4 + 卖 3

    ASSERT_TRUE(book.add(mk(3, 100, 2, Side::Sell)));
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 9);

    ASSERT_TRUE(book.cancel(OID(1)));
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 5); // 只剩卖 3 + 卖 2

    ASSERT_TRUE(book.cancel(OID(2)));
    expect_order(book.best_ask(), 3, 100, 2, Side::Sell); // 卖盘队头换人
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 2);

    ASSERT_TRUE(book.cancel(OID(3)));
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 0); // 双侧都空 -> 0
}

TEST(OrderBookW2, CancelOneSideKeepsTheOtherAtSamePrice) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 4, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 100, 3, Side::Sell)));
    ASSERT_TRUE(book.cancel(OID(1)));
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 3); // 卖盘还在
    expect_order(book.best_ask(), 2, 100, 3, Side::Sell);
    EXPECT_FALSE(book.best_bid().has_value()); // 买盘已空
}

// ---------------------------------------------------------------------------
// 差分对拍：OrderBook vs NaiveOrderBook
// ---------------------------------------------------------------------------

namespace {

constexpr int                   kNumIds = 10;
constexpr std::int64_t          kIdBase = 1000;
const std::vector<std::int64_t> kPrices{90, 100, 101, 102, 103, 104};

OrderId OID_OF(int i) { return OID(kIdBase + i); }

} // namespace

class Differential : public ::testing::TestWithParam<int> {};

TEST_P(Differential, RandomOpSequence) {
    const int    seed = GetParam();
    std::mt19937 rng(static_cast<std::uint32_t>(seed));

    OrderBook         fast;
    NaiveOrderBook    slow;
    std::vector<bool> live(static_cast<std::size_t>(kNumIds), false);

    const auto same_order = [](const std::optional<Order> &x, const std::optional<Order> &y) {
        if (x.has_value() != y.has_value()) return false;
        if (!x.has_value()) return true;
        return x->price == y->price && x->orderId == y->orderId && x->quantity == y->quantity &&
               x->side == y->side;
    };

    std::string first_diff;
    const auto  check_state = [&]() {
        if (!first_diff.empty()) return;
        if (fast.size() != slow.size()) {
            first_diff = "size 不一致: fast=" + std::to_string(fast.size()) +
                         " slow=" + std::to_string(slow.size());
            return;
        }
        if (!same_order(fast.best_bid(), slow.best_bid())) {
            first_diff = "best_bid 不一致";
            return;
        }
        if (!same_order(fast.best_ask(), slow.best_ask())) {
            first_diff = "best_ask 不一致";
            return;
        }
        for (int i = 0; i < kNumIds; ++i) {
            if (!same_order(fast.find(OID_OF(i)), slow.find(OID_OF(i)))) {
                std::ostringstream os;
                os << "find(id=" << kIdBase + i << ") 不一致";
                first_diff = os.str();
                return;
            }
        }
        for (std::int64_t p : kPrices) {
            if (fast.quantity_at(Price{p}).get() != slow.quantity_at(Price{p}).get()) {
                std::ostringstream os;
                os << "quantity_at(" << p << ") 不一致";
                first_diff = os.str();
                return;
            }
        }
        if (fast.quantity_at(Price{777}).get() != slow.quantity_at(Price{777}).get()) {
            first_diff = "quantity_at(777) 不一致";
        }
    };

    const auto random_price = [&]() -> std::int64_t {
        if (rng() % 23 == 0) return -static_cast<std::int64_t>(rng() % 4) - 1; // 偶尔非法价
        return kPrices[rng() % kPrices.size()];
    };
    const auto random_qty = [&]() -> std::int64_t {
        const int t = static_cast<int>(rng() % 18);
        if (t == 0) return 0;                                         // 偶尔 0
        if (t == 1) return -static_cast<std::int64_t>(rng() % 3 + 1); // 偶尔负数
        return static_cast<std::int64_t>(1 + rng() % 9);
    };

    constexpr int kOps = 600;
    for (int step = 0; step < kOps; ++step) {
        const int   i = static_cast<int>(rng() % kNumIds);
        bool        a = false;
        bool        b = false;
        std::string desc;

        const int roll = static_cast<int>(rng() % 100);
        if (roll < 40) {
            const std::int64_t price = random_price();
            const std::int64_t qty   = random_qty();
            const Side         side  = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            desc                     = "add";
            a                        = fast.add(Order{Price{price}, OID_OF(i), Quantity{qty}, side});
            b                        = slow.add(Order{Price{price}, OID_OF(i), Quantity{qty}, side});
            if (a == b && a) live[i] = true;
        }
        else if (roll < 60) {
            desc = "cancel";
            a    = fast.cancel(OID_OF(i));
            b    = slow.cancel(OID_OF(i));
            if (a == b && a) live[i] = false;
        }
        else if (roll < 80) {
            const std::int64_t qty = random_qty();
            desc                   = "modify(qty)";
            a                      = fast.modify(OID_OF(i), Quantity{qty});
            b                      = slow.modify(OID_OF(i), Quantity{qty});
        }
        else {
            const std::int64_t price = random_price();
            const std::int64_t qty   = random_qty();
            desc                     = "modify(price,qty)";
            a                        = fast.modify(OID_OF(i), Price{price}, Quantity{qty});
            b                        = slow.modify(OID_OF(i), Price{price}, Quantity{qty});
        }

        if (a != b) {
            ADD_FAILURE() << "seed=" << seed << " step=" << step << " [" << desc
                          << "] id=" << kIdBase + i << "：返回值不一致 fast=" << a << " slow=" << b;
            return;
        }

        check_state();
        if (!first_diff.empty()) {
            ADD_FAILURE() << "seed=" << seed << " step=" << step << " [" << desc
                          << "] id=" << kIdBase + i << " 操作后：" << first_diff;
            return;
        }
    }
    // 收尾再对一次状态（live 只是本地影子，不影响簿）
    (void)live;
    check_state();
    if (!first_diff.empty()) {
        ADD_FAILURE() << "seed=" << seed << " 最终状态：" << first_diff;
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, Differential, ::testing::Range(1, 41));
