// W3 验收测试（AI 隔离编写，只作为正确性裁判）。
//
// 内容分四部分：
//   1. MatchingW3.*            确定性单元测试：五种订单类型 / price-time 撮合 /
//                              成交价 / 部分成交挂簿 / 拒绝无副作用 / 事件流等。
//   2. MatchingW3.Lifetime*    生命周期与内存/UB 压力：大量建簿-扫簿-撤单-析构，
//                              配合 ASan/LSan/UBSan 抓 use-after-free / 泄漏 / UB。
//   3. Differential/*          随机差分对拍：把 MatchingEngine 与慢速参考撮合
//                              NaiveMatchingEngine 喂相同 submit/cancel 序列，逐步
//                              比对 SubmitResult / 成交序列 / 簿状态。
//
// 语义契约见 docs/W3_MATCHING_GUIDE.md 的“判据”一节，两者必须逐字一致。
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "Maelstrom/matching_engine.h"
#include "Maelstrom/order_book.h"
#include "naive_matching_engine.h"

using namespace matching_engine;
using namespace maelstrom;
using maelstrom::test_support::NaiveMatchingEngine;

namespace {

OrderId OID(std::int64_t v) { return OrderId{v}; }

// Order 字段顺序：{Price, OrderId, Quantity, Side, OrderType}
Order mk(std::int64_t id, std::int64_t price, std::int64_t qty, Side side,
         OrderType type = OrderType::Limit) {
    return Order{Price{price}, OID(id), Quantity{qty}, side, type};
}

void expect_order(const std::optional<Order> &got, std::int64_t id, std::int64_t price,
                  std::int64_t qty, Side side) {
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->orderId, OID(id));
    EXPECT_EQ(got->price.get(), price);
    EXPECT_EQ(got->quantity.get(), qty);
    EXPECT_EQ(got->side, side);
}

void expect_no_order(const std::optional<Order> &got) { EXPECT_FALSE(got.has_value()); }

void expect_trade(const Trade &t, std::int64_t aggressor, std::int64_t passive, std::int64_t price,
                  std::int64_t qty, Side aggressor_side) {
    EXPECT_EQ(t.aggressive_id, OID(aggressor));
    EXPECT_EQ(t.passive_id, OID(passive));
    EXPECT_EQ(t.price.get(), price);
    EXPECT_EQ(t.quantity.get(), qty);
    EXPECT_EQ(t.aggressor_side, aggressor_side);
}

void expect_result(const SubmitResult &r, OrderOutcome outcome, Quantity filled, Quantity resting,
                   std::size_t trade_count, RejectReason reason = RejectReason::None) {
    EXPECT_EQ(r.outcome, outcome);
    EXPECT_EQ(r.reason, reason);
    EXPECT_EQ(r.filled.get(), filled.get());
    EXPECT_EQ(r.resting.get(), resting.get());
    EXPECT_EQ(r.trade_count, trade_count);
}

// 一个把成交记进 vector 的回调。
struct TradeLog {
    std::vector<Trade> trades;
    void               operator()(const Trade &t) { trades.push_back(t); }
};

} // namespace

// ===========================================================================
// 1. 校验与拒绝
// ===========================================================================

TEST(MatchingW3, RejectInvalidQuantityAllTypes) {
    const OrderType types[] = {OrderType::Limit, OrderType::Market, OrderType::Ioc, OrderType::Fok,
                               OrderType::PostOnly};
    for (OrderType ty : types) {
        MatchingEngine eng;
        TradeLog       log;
        eng.set_trade_handler([&](const Trade &t) { log(t); });

        for (std::int64_t q : {0, -1, -100}) {
            SubmitResult r = eng.submit(mk(1, 100, q, Side::Buy, ty));
            expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                          RejectReason::InvalidQuantity);
            EXPECT_FALSE(r.accepted());
        }
        EXPECT_EQ(eng.book().size(), 0u);
        EXPECT_TRUE(log.trades.empty());
    }
}

TEST(MatchingW3, RejectInvalidPriceForPricedTypes) {
    const OrderType priced[] = {OrderType::Limit, OrderType::Ioc, OrderType::Fok,
                                OrderType::PostOnly};
    for (OrderType ty : priced) {
        MatchingEngine eng;
        for (std::int64_t p : {0, -1, -99}) {
            SubmitResult r = eng.submit(mk(1, p, 10, Side::Buy, ty));
            expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                          RejectReason::InvalidPrice);
        }
        EXPECT_EQ(eng.book().size(), 0u);
    }
}

TEST(MatchingW3, MarketIgnoresPriceField) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted()); // 先垫一笔卖单
    // Market 的 price 被忽略：0 与负数都应正常当市价单处理
    SubmitResult r0 = eng.submit(mk(2, 0, 3, Side::Buy, OrderType::Market));
    expect_result(r0, OrderOutcome::Filled, Quantity{3}, Quantity{0}, 1);
    SubmitResult r1 = eng.submit(mk(3, -99, 2, Side::Buy, OrderType::Market));
    expect_result(r1, OrderOutcome::Filled, Quantity{2}, Quantity{0}, 1);
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, RejectDuplicateIdLeavesBookUntouched) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 10, Side::Buy)).outcome, OrderOutcome::Rested);

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    SubmitResult r = eng.submit(mk(1, 101, 7, Side::Sell));
    expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                  RejectReason::DuplicateOrderId);

    expect_order(eng.book().best_bid(), 1, 100, 10, Side::Buy);
    EXPECT_EQ(eng.book().size(), 1u);
    EXPECT_TRUE(log.trades.empty());
}

TEST(MatchingW3, RejectDuplicateIdAgainstPartiallyFilledRemainder) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 4, Side::Sell)).outcome, OrderOutcome::Rested);
    // id=2 买 10 吃 4 后余 6 挂簿
    SubmitResult first = eng.submit(mk(2, 100, 10, Side::Buy));
    expect_result(first, OrderOutcome::PartiallyFilled, Quantity{4}, Quantity{6}, 1);

    SubmitResult again = eng.submit(mk(2, 100, 1, Side::Buy));
    EXPECT_EQ(again.outcome, OrderOutcome::Rejected);
    EXPECT_EQ(again.reason, RejectReason::DuplicateOrderId);
    expect_order(eng.book().best_bid(), 2, 100, 6, Side::Buy); // 原单未受影响
}

TEST(MatchingW3, ValidationPrecedenceQuantityBeforeIdBeforePrice) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 5, Side::Buy)).outcome, OrderOutcome::Rested);

    // 数量非法优先于重复 id 与价格
    SubmitResult a = eng.submit(mk(1, -5, 0, Side::Buy));
    EXPECT_EQ(a.reason, RejectReason::InvalidQuantity);
    // 重复 id 优先于价格
    SubmitResult b = eng.submit(mk(1, -5, 5, Side::Buy));
    EXPECT_EQ(b.reason, RejectReason::DuplicateOrderId);
}

TEST(MatchingW3, RejectedIdRemainsUsable) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 5, Side::Sell)).outcome, OrderOutcome::Rested);

    // id=2 作为 POST-ONLY 买会被拒（id 不入簿）
    SubmitResult rej = eng.submit(mk(2, 100, 5, Side::Buy, OrderType::PostOnly));
    EXPECT_EQ(rej.reason, RejectReason::PostOnlyWouldCross);
    EXPECT_FALSE(eng.book().find(OID(2)).has_value());

    // 同一个 id 换普通限价单应能正常挂上
    SubmitResult ok = eng.submit(mk(2, 99, 5, Side::Buy));
    expect_result(ok, OrderOutcome::Rested, Quantity{0}, Quantity{5}, 0);
    expect_order(eng.book().best_bid(), 2, 99, 5, Side::Buy);
}

// ===========================================================================
// 2. 限价单连续撮合
// ===========================================================================

TEST(MatchingW3, LimitBuyFullCrossUsesPassivePrice) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 10, Side::Sell)).outcome, OrderOutcome::Rested);

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });

    SubmitResult r = eng.submit(mk(2, 105, 4, Side::Buy)); // 主动买比卖价高 -> 价格改善
    expect_result(r, OrderOutcome::Filled, Quantity{4}, Quantity{0}, 1);
    ASSERT_EQ(log.trades.size(), 1u);
    expect_trade(log.trades[0], 2, 1, 100, 4, Side::Buy); // 成交价 = 被动方 100
    expect_order(eng.book().best_ask(), 1, 100, 6, Side::Sell);
    expect_no_order(eng.book().best_bid());
    EXPECT_EQ(eng.book().size(), 1u);
}

TEST(MatchingW3, LimitBuySweepsLevelsInPriceOrder) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 103, 4, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 101, 5, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(3, 102, 3, Side::Sell)).accepted());

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });

    SubmitResult r = eng.submit(mk(9, 103, 10, Side::Buy)); // 限价 103 依次吃 101/102/103
    expect_result(r, OrderOutcome::Filled, Quantity{10}, Quantity{0}, 3);
    ASSERT_EQ(log.trades.size(), 3u);
    expect_trade(log.trades[0], 9, 2, 101, 5, Side::Buy); // 先吃最优价 101
    expect_trade(log.trades[1], 9, 3, 102, 3, Side::Buy);
    expect_trade(log.trades[2], 9, 1, 103, 2, Side::Buy); // 103 只吃到 2
    expect_order(eng.book().best_ask(), 1, 103, 2, Side::Sell);
    EXPECT_EQ(eng.book().size(), 1u);
}

TEST(MatchingW3, LimitBuyPartialRestsRemainder) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 4, Side::Sell)).outcome, OrderOutcome::Rested);

    SubmitResult r = eng.submit(mk(2, 100, 10, Side::Buy));
    expect_result(r, OrderOutcome::PartiallyFilled, Quantity{4}, Quantity{6}, 1);
    expect_no_order(eng.book().best_ask());
    expect_order(eng.book().best_bid(), 2, 100, 6, Side::Buy);
    EXPECT_EQ(eng.book().size(), 1u);
}

TEST(MatchingW3, LimitNoCrossRests) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 101, 5, Side::Sell)).outcome, OrderOutcome::Rested);

    SubmitResult r = eng.submit(mk(2, 100, 7, Side::Buy)); // 买价低于卖价 -> 不成交
    expect_result(r, OrderOutcome::Rested, Quantity{0}, Quantity{7}, 0);
    expect_order(eng.book().best_bid(), 2, 100, 7, Side::Buy);
    expect_order(eng.book().best_ask(), 1, 101, 5, Side::Sell);
}

TEST(MatchingW3, LimitSellSymmetric) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 5, Side::Buy)).outcome, OrderOutcome::Rested);

    SubmitResult r = eng.submit(mk(2, 99, 8, Side::Sell));
    expect_result(r, OrderOutcome::PartiallyFilled, Quantity{5}, Quantity{3}, 1);
    expect_no_order(eng.book().best_bid());
    expect_order(eng.book().best_ask(), 2, 99, 3, Side::Sell);
}

TEST(MatchingW3, LimitEqualPriceCrosses) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 5, Side::Sell)).outcome, OrderOutcome::Rested);
    SubmitResult r = eng.submit(mk(2, 100, 5, Side::Buy)); // 价格相等即成交
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, SamePriceIsFifo) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 3, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 100, 4, Side::Sell)).accepted());

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    SubmitResult r = eng.submit(mk(9, 100, 5, Side::Buy));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 2);
    ASSERT_EQ(log.trades.size(), 2u);
    expect_trade(log.trades[0], 9, 1, 100, 3, Side::Buy); // 先到先成交
    expect_trade(log.trades[1], 9, 2, 100, 2, Side::Buy);
    expect_order(eng.book().best_ask(), 2, 100, 2, Side::Sell);
}

TEST(MatchingW3, PricePriorityBeatsArrivalTime) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 101, 5, Side::Sell)).accepted()); // 先到但价差
    ASSERT_TRUE(eng.submit(mk(2, 100, 5, Side::Sell)).accepted()); // 后到但价优

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    ASSERT_EQ(eng.submit(mk(9, 101, 10, Side::Buy)).outcome, OrderOutcome::Filled);
    ASSERT_EQ(log.trades.size(), 2u);
    expect_trade(log.trades[0], 9, 2, 100, 5, Side::Buy); // 价优者先成交
    expect_trade(log.trades[1], 9, 1, 101, 5, Side::Buy);
}

TEST(MatchingW3, PassivePartialFillKeepsQueuePosition) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 10, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 100, 10, Side::Sell)).accepted());

    ASSERT_EQ(eng.submit(mk(8, 100, 5, Side::Buy)).filled.get(), 5);
    expect_order(eng.book().best_ask(), 1, 100, 5, Side::Sell); // id1 减量但仍排队首

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    ASSERT_EQ(eng.submit(mk(9, 100, 3, Side::Buy)).filled.get(), 3);
    ASSERT_EQ(log.trades.size(), 1u);
    expect_trade(log.trades[0], 9, 1, 100, 3, Side::Buy); // 仍打 id1，不给 id2
}

// ===========================================================================
// 3. 市价单
// ===========================================================================

TEST(MatchingW3, MarketBuySweepsThenCancelsRemainder) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 101, 5, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 102, 5, Side::Sell)).accepted());

    SubmitResult r = eng.submit(mk(9, 0, 20, Side::Buy, OrderType::Market));
    expect_result(r, OrderOutcome::Cancelled, Quantity{10}, Quantity{0}, 2);
    EXPECT_EQ(eng.book().size(), 0u); // 余量 10 被取消，不挂簿
    expect_no_order(eng.book().best_bid());
}

TEST(MatchingW3, MarketOnEmptyBookCancels) {
    MatchingEngine eng;
    SubmitResult   r = eng.submit(mk(1, 0, 5, Side::Buy, OrderType::Market));
    expect_result(r, OrderOutcome::Cancelled, Quantity{0}, Quantity{0}, 0);
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, MarketSellSweepsAllLevels) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 3, Side::Buy)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 99, 4, Side::Buy)).accepted());

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    SubmitResult r = eng.submit(mk(9, 0, 100, Side::Sell, OrderType::Market));
    expect_result(r, OrderOutcome::Cancelled, Quantity{7}, Quantity{0}, 2);
    ASSERT_EQ(log.trades.size(), 2u);
    expect_trade(log.trades[0], 9, 1, 100, 3, Side::Sell); // 先吃最高买价
    expect_trade(log.trades[1], 9, 2, 99, 4, Side::Sell);
    EXPECT_EQ(eng.book().size(), 0u);
}

// ===========================================================================
// 4. IOC
// ===========================================================================

TEST(MatchingW3, IocPartialFillsThenCancels) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted());

    SubmitResult r = eng.submit(mk(2, 100, 8, Side::Buy, OrderType::Ioc));
    expect_result(r, OrderOutcome::Cancelled, Quantity{5}, Quantity{0}, 1);
    EXPECT_EQ(eng.book().size(), 0u); // 余量 3 取消，不挂簿
}

TEST(MatchingW3, IocFullyFills) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 8, Side::Sell)).accepted());
    SubmitResult r = eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Ioc));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
    expect_order(eng.book().best_ask(), 1, 100, 3, Side::Sell);
}

TEST(MatchingW3, IocNoCrossCancels) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 101, 5, Side::Sell)).accepted());
    SubmitResult r = eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Ioc));
    expect_result(r, OrderOutcome::Cancelled, Quantity{0}, Quantity{0}, 0);
    EXPECT_EQ(eng.book().size(), 1u);
    expect_order(eng.book().best_ask(), 1, 101, 5, Side::Sell);
}

// ===========================================================================
// 5. FOK
// ===========================================================================

TEST(MatchingW3, FokFillableExactly) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted());
    SubmitResult r = eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Fok));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, FokFillableAcrossLevels) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 101, 4, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 100, 3, Side::Sell)).accepted());

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    SubmitResult r = eng.submit(mk(9, 101, 7, Side::Buy, OrderType::Fok));
    expect_result(r, OrderOutcome::Filled, Quantity{7}, Quantity{0}, 2);
    ASSERT_EQ(log.trades.size(), 2u);
    expect_trade(log.trades[0], 9, 2, 100, 3, Side::Buy);
    expect_trade(log.trades[1], 9, 1, 101, 4, Side::Buy);
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, FokNotFillableRejectedWithoutSideEffects) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 3, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 101, 4, Side::Sell)).accepted());

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    SubmitResult r = eng.submit(mk(9, 101, 8, Side::Buy, OrderType::Fok));
    expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                  RejectReason::FokNotFillable);
    EXPECT_TRUE(log.trades.empty());  // 无成交
    EXPECT_EQ(eng.book().size(), 2u); // 簿未被改动
    expect_order(eng.book().best_ask(), 1, 100, 3, Side::Sell);
}

TEST(MatchingW3, FokRespectsPriceLimit) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 3, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 105, 100, Side::Sell)).accepted());
    // 限价 103 只能吃到 100 档的 3，不足 10 -> 拒绝
    SubmitResult r = eng.submit(mk(9, 103, 10, Side::Buy, OrderType::Fok));
    expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                  RejectReason::FokNotFillable);
    EXPECT_EQ(eng.book().size(), 2u);
}

TEST(MatchingW3, FokSellSide) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Buy)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 99, 5, Side::Buy)).accepted());
    SubmitResult r = eng.submit(mk(9, 99, 10, Side::Sell, OrderType::Fok));
    expect_result(r, OrderOutcome::Filled, Quantity{10}, Quantity{0}, 2);
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, FokIsPricedType) {
    // FOK 属于“限价类”：必须给出正价格（市价 FOK 由扩展模块的 Market+TimeInForce 组合表达）。
    MatchingEngine eng;
    SubmitResult   r = eng.submit(mk(1, 0, 5, Side::Buy, OrderType::Fok));
    expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                  RejectReason::InvalidPrice);
}

// ===========================================================================
// 6. POST-ONLY
// ===========================================================================

TEST(MatchingW3, PostOnlyRestsWhenNoCross) {
    MatchingEngine eng;
    SubmitResult   r = eng.submit(mk(1, 100, 5, Side::Buy, OrderType::PostOnly));
    expect_result(r, OrderOutcome::Rested, Quantity{0}, Quantity{5}, 0);
    expect_order(eng.book().best_bid(), 1, 100, 5, Side::Buy);
}

TEST(MatchingW3, PostOnlyRejectsWhenWouldCrossAtEqualPrice) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted());

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    SubmitResult r = eng.submit(mk(2, 100, 5, Side::Buy, OrderType::PostOnly));
    expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                  RejectReason::PostOnlyWouldCross);
    EXPECT_TRUE(log.trades.empty());
    EXPECT_EQ(eng.book().size(), 1u);
}

TEST(MatchingW3, PostOnlyRejectsWhenStrictlyBetterBuy) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted());
    SubmitResult r = eng.submit(mk(2, 103, 5, Side::Buy, OrderType::PostOnly));
    EXPECT_EQ(r.reason, RejectReason::PostOnlyWouldCross);
}

TEST(MatchingW3, PostOnlyRestsWhenWorseThanBestAsk) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 101, 5, Side::Sell)).accepted());
    SubmitResult r = eng.submit(mk(2, 100, 5, Side::Buy, OrderType::PostOnly));
    expect_result(r, OrderOutcome::Rested, Quantity{0}, Quantity{5}, 0);
    expect_order(eng.book().best_bid(), 2, 100, 5, Side::Buy);
    expect_order(eng.book().best_ask(), 1, 101, 5, Side::Sell);
}

TEST(MatchingW3, PostOnlySellSide) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Buy)).accepted());
    EXPECT_EQ(eng.submit(mk(2, 100, 5, Side::Sell, OrderType::PostOnly)).reason,
              RejectReason::PostOnlyWouldCross);
    SubmitResult ok = eng.submit(mk(3, 101, 5, Side::Sell, OrderType::PostOnly));
    expect_result(ok, OrderOutcome::Rested, Quantity{0}, Quantity{5}, 0);
    expect_order(eng.book().best_ask(), 3, 101, 5, Side::Sell);
}

// ===========================================================================
// 7. 事件流与簿状态
// ===========================================================================

TEST(MatchingW3, NoHandlerStillReturnsTradeCount) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted());
    SubmitResult r = eng.submit(mk(2, 100, 5, Side::Buy)); // 未设置 handler
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
}

TEST(MatchingW3, HandlerCanBeReplaced) {
    MatchingEngine eng;
    TradeLog       a;
    TradeLog       b;
    eng.set_trade_handler([&](const Trade &t) { a(t); });
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(2, 100, 5, Side::Buy)).accepted());
    EXPECT_EQ(a.trades.size(), 1u);
    EXPECT_EQ(b.trades.size(), 0u);

    eng.set_trade_handler([&](const Trade &t) { b(t); });
    ASSERT_TRUE(eng.submit(mk(3, 100, 4, Side::Sell)).accepted());
    ASSERT_TRUE(eng.submit(mk(4, 100, 4, Side::Buy)).accepted());
    EXPECT_EQ(a.trades.size(), 1u); // 旧 handler 不再收到
    EXPECT_EQ(b.trades.size(), 1u);
}

TEST(MatchingW3, CancelDelegatesAndEmitsNoTrade) {
    MatchingEngine eng;
    TradeLog       log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    ASSERT_EQ(eng.submit(mk(1, 100, 5, Side::Buy)).outcome, OrderOutcome::Rested);

    EXPECT_TRUE(eng.cancel(OID(1)));
    EXPECT_FALSE(eng.cancel(OID(1)));
    EXPECT_FALSE(eng.cancel(OID(999)));
    EXPECT_EQ(eng.book().size(), 0u);
    EXPECT_TRUE(log.trades.empty());
}

TEST(MatchingW3, QuantityAccountingPerType) {
    // 每类订单量守恒：
    //  Limit/PostOnly 满额分配 -> filled + resting == qty
    //  Market/IOC       残余取消 -> resting == 0 且 filled <= qty
    //  FOK              要么全额成交，要么整单拒绝
    const std::int64_t book_qty = 30;
    struct Case {
        OrderType    type;
        std::int64_t qty;
        std::int64_t price;
    };
    const Case cases[] = {{OrderType::Limit, 50, 100}, {OrderType::Ioc, 50, 100}, {OrderType::Fok, 20, 100}, {OrderType::Market, 7, 100}, {OrderType::PostOnly, 5, 99}};
    for (const Case &c : cases) {
        MatchingEngine eng;
        ASSERT_TRUE(eng.submit(mk(1, 100, book_qty, Side::Sell)).accepted());
        SubmitResult r = eng.submit(mk(2, c.price, c.qty, Side::Buy, c.type));
        ASSERT_TRUE(r.accepted()) << "type=" << int(c.type);
        EXPECT_LE(r.filled.get(), c.qty) << "type=" << int(c.type);
        switch (c.type) {
        case OrderType::Limit:
        case OrderType::PostOnly:
            EXPECT_EQ(r.filled.get() + r.resting.get(), c.qty) << "type=" << int(c.type);
            break;
        case OrderType::Market:
        case OrderType::Ioc:
            EXPECT_EQ(r.resting.get(), 0) << "type=" << int(c.type);
            break;
        case OrderType::Fok:
            EXPECT_EQ(r.filled.get(), c.qty) << "type=" << int(c.type);
            EXPECT_EQ(r.resting.get(), 0) << "type=" << int(c.type);
            break;
        }
    }
}

TEST(MatchingW3, BookReflectsRestingRemainderUnderAggressorId) {
    MatchingEngine eng;
    ASSERT_EQ(eng.submit(mk(1, 100, 4, Side::Sell)).outcome, OrderOutcome::Rested);
    ASSERT_EQ(eng.submit(mk(2, 100, 10, Side::Buy)).outcome, OrderOutcome::PartiallyFilled);

    auto rest = eng.book().find(OID(2));
    expect_order(rest, 2, 100, 6, Side::Buy);
    EXPECT_EQ(eng.book().size(), 1u);
    EXPECT_EQ(eng.book().quantity_at(Price{100}).get(), 6);
}

TEST(MatchingW3, IdReusableAfterFullFillAndAfterCancel) {
    MatchingEngine eng;
    ASSERT_TRUE(eng.submit(mk(1, 100, 5, Side::Sell)).accepted());
    ASSERT_EQ(eng.submit(mk(2, 100, 5, Side::Buy)).outcome, OrderOutcome::Filled);
    // 完全成交后 id 不在簿，可复用
    ASSERT_EQ(eng.submit(mk(2, 99, 3, Side::Buy)).outcome, OrderOutcome::Rested);

    ASSERT_TRUE(eng.cancel(OID(2)));
    ASSERT_EQ(eng.submit(mk(2, 98, 3, Side::Buy)).outcome, OrderOutcome::Rested);
    expect_order(eng.book().best_bid(), 2, 98, 3, Side::Buy);
}

TEST(MatchingW3, EmptyEngineQueries) {
    MatchingEngine eng;
    EXPECT_EQ(eng.book().size(), 0u);
    expect_no_order(eng.book().best_bid());
    expect_no_order(eng.book().best_ask());
    EXPECT_EQ(eng.book().quantity_at(Price{100}).get(), 0);
}

TEST(MatchingW3, EngineIsMoveOnly) {
    static_assert(!std::is_copy_constructible<MatchingEngine>::value, "must not be copyable");
    static_assert(!std::is_copy_assignable<MatchingEngine>::value, "must not be copyable");
    static_assert(std::is_move_constructible<MatchingEngine>::value, "must be movable");
    static_assert(std::is_move_assignable<MatchingEngine>::value, "must be movable");

    MatchingEngine a;
    ASSERT_TRUE(a.submit(mk(1, 100, 5, Side::Sell)).accepted());
    MatchingEngine b(std::move(a));
    SubmitResult   r = b.submit(mk(2, 100, 5, Side::Buy));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
}

// ===========================================================================
// 8. 生命周期 / 内存 / UB 压力（配合 ASan/LSan/UBSan 才有意义）
// ===========================================================================

TEST(MatchingW3, LifetimeStressManyAllocsAndSweeps) {
    std::mt19937 rng(20260915u);
    for (int round = 0; round < 40; ++round) {
        MatchingEngine eng;
        std::int64_t   id = 1;
        for (int i = 0; i < 150; ++i) {
            const std::int64_t price = 95 + static_cast<std::int64_t>(rng() % 12);
            const std::int64_t qty   = 1 + static_cast<std::int64_t>(rng() % 20);
            const Side         side  = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            OrderType          type  = OrderType::Limit;
            if (rng() % 3 == 0) {
                type = OrderType::Limit;
            }
            else if (rng() % 4 == 0) {
                type = OrderType::Ioc;
            }
            else {
                type = OrderType::Market;
            }
            [[maybe_unused]] const SubmitResult r = eng.submit(mk(id++, price, qty, side, type));
            EXPECT_GE(eng.book().size(), 0u); // 每步触碰簿，逼出潜在的 UB
        }
        // 全撤：每个在簿 id 撤一次，制造大量释放与空档删除
        for (std::int64_t i = 1; i <= id; ++i) {
            eng.cancel(OID(i));
        }
        EXPECT_EQ(eng.book().size(), 0u);
    }
}

TEST(MatchingW3, LifetimeDeepSweepSamePrice) {
    MatchingEngine eng;
    constexpr int  kOrders = 400;
    for (int i = 0; i < kOrders; ++i) {
        ASSERT_TRUE(eng.submit(mk(i + 1, 100, 1, Side::Sell)).accepted());
    }
    EXPECT_EQ(eng.book().size(), static_cast<std::size_t>(kOrders));

    TradeLog log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    SubmitResult r = eng.submit(mk(99999, 100, kOrders, Side::Buy));
    expect_result(r, OrderOutcome::Filled, Quantity{kOrders}, Quantity{0},
                  static_cast<std::size_t>(kOrders));
    ASSERT_EQ(log.trades.size(), static_cast<std::size_t>(kOrders));
    for (int i = 0; i < kOrders; ++i) { // FIFO 顺序严格
        EXPECT_EQ(log.trades[static_cast<std::size_t>(i)].passive_id, OID(i + 1));
    }
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, LifetimeManyLevelsThenTeardown) {
    MatchingEngine eng;
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(eng.submit(mk(1000 + i, 2000 + i, 1, Side::Sell)).accepted());
        ASSERT_TRUE(eng.submit(mk(5000 + i, 600 - i, 1, Side::Buy)).accepted());
    }
    EXPECT_EQ(eng.book().size(), 1000u);
    // 一大笔市价单扫空卖盘，留下买盘，触发大量价位删除
    SubmitResult r = eng.submit(mk(1, 0, 100000, Side::Buy, OrderType::Market));
    EXPECT_EQ(r.filled.get(), 500);
    EXPECT_EQ(eng.book().size(), 500u);
    for (int i = 0; i < 500; ++i) { // 撤掉全部买盘，随后析构
        EXPECT_TRUE(eng.cancel(OID(5000 + i)));
    }
    EXPECT_EQ(eng.book().size(), 0u);
}

TEST(MatchingW3, LifetimeRejectedLoopHasNoResidue) {
    MatchingEngine eng;
    TradeLog       log;
    eng.set_trade_handler([&](const Trade &t) { log(t); });
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(eng.submit(mk(i + 1, 0, 10, Side::Buy)).accepted());  // 非法价
        EXPECT_FALSE(eng.submit(mk(i + 1, 100, 0, Side::Buy)).accepted()); // 非法量
        EXPECT_FALSE(eng.submit(mk(i + 1, 100, 5, Side::Buy, OrderType::Fok)).accepted());
    }
    EXPECT_EQ(eng.book().size(), 0u);
    EXPECT_TRUE(log.trades.empty());
}

// ===========================================================================
// 9. 订单簿撮合原语（consume_best / matchable_quantity）直接验收
// ===========================================================================

TEST(BookPrimitivesW3, ConsumeBestEmptySideReturnsNullopt) {
    OrderBook book;
    EXPECT_FALSE(book.consume_best(Side::Buy, Quantity{5}).has_value());
    EXPECT_FALSE(book.consume_best(Side::Sell, Quantity{5}).has_value());
    EXPECT_FALSE(book.consume_best(Side::Buy, Quantity{0}).has_value());
    EXPECT_FALSE(book.consume_best(Side::Buy, Quantity{-1}).has_value());
}

TEST(BookPrimitivesW3, ConsumeBestPartialReducesHeadKeepsQueue) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 10, Side::Sell)));
    ASSERT_TRUE(book.add(mk(2, 100, 10, Side::Sell)));

    auto f = book.consume_best(Side::Sell, Quantity{4});
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->passive_id, OID(1));
    EXPECT_EQ(f->price.get(), 100);
    EXPECT_EQ(f->quantity.get(), 4);
    EXPECT_EQ(book.size(), 2u); // 队头未吃光，笔数不变
    expect_order(book.best_ask(), 1, 100, 6, Side::Sell);
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 16);
}

TEST(BookPrimitivesW3, ConsumeBestFullRemovesHeadAndEmptyLevel) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Sell)));
    ASSERT_TRUE(book.add(mk(2, 101, 5, Side::Sell)));

    auto f = book.consume_best(Side::Sell, Quantity{100}); // 超过队头量 -> 只吃队头
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->passive_id, OID(1));
    EXPECT_EQ(f->quantity.get(), 5);
    EXPECT_EQ(book.size(), 1u);
    expect_order(book.best_ask(), 2, 101, 5, Side::Sell); // 100 档已清空
    EXPECT_EQ(book.quantity_at(Price{100}).get(), 0);
}

TEST(BookPrimitivesW3, ConsumeBestBuySideUsesHighestPrice) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 99, 3, Side::Buy)));
    ASSERT_TRUE(book.add(mk(2, 103, 3, Side::Buy)));
    auto f = book.consume_best(Side::Buy, Quantity{2});
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->passive_id, OID(2)); // 最高买价优先
    EXPECT_EQ(f->price.get(), 103);
    expect_order(book.best_bid(), 2, 103, 1, Side::Buy);
}

TEST(BookPrimitivesW3, ConsumeBestIsFifoWithinLevel) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 2, Side::Sell)));
    ASSERT_TRUE(book.add(mk(2, 100, 2, Side::Sell)));
    EXPECT_EQ(book.consume_best(Side::Sell, Quantity{2})->passive_id, OID(1));
    EXPECT_EQ(book.consume_best(Side::Sell, Quantity{2})->passive_id, OID(2));
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(BookPrimitivesW3, MatchableQuantityWholeSide) {
    OrderBook book;
    ASSERT_TRUE(book.add(mk(1, 100, 5, Side::Sell)));
    ASSERT_TRUE(book.add(mk(2, 101, 7, Side::Sell)));
    ASSERT_TRUE(book.add(mk(3, 90, 9, Side::Buy)));

    EXPECT_EQ(book.matchable_quantity(Side::Sell, std::nullopt).get(), 12);
    EXPECT_EQ(book.matchable_quantity(Side::Buy, std::nullopt).get(), 9);
    EXPECT_EQ(book.matchable_quantity(Side::Sell, Price{100}).get(), 5);  // <= 100
    EXPECT_EQ(book.matchable_quantity(Side::Sell, Price{101}).get(), 12); // <= 101
    EXPECT_EQ(book.matchable_quantity(Side::Sell, Price{99}).get(), 0);   // 无可成交
    EXPECT_EQ(book.matchable_quantity(Side::Buy, Price{90}).get(), 9);    // >= 90
    EXPECT_EQ(book.matchable_quantity(Side::Buy, Price{100}).get(), 0);   // 买价 90 < 100，不交叉
}

TEST(BookPrimitivesW3, MatchableQuantityEmptyBook) {
    OrderBook book;
    EXPECT_EQ(book.matchable_quantity(Side::Sell, Price{100}).get(), 0);
    EXPECT_EQ(book.matchable_quantity(Side::Buy, std::nullopt).get(), 0);
}

// ===========================================================================
// 10. 差分对拍：MatchingEngine vs NaiveMatchingEngine
// ===========================================================================
namespace {

constexpr int                   kNumIds = 12;
constexpr std::int64_t          kIdBase = 7000;
const std::vector<std::int64_t> kPrices{99, 100, 101, 102, 103, 104};

OrderId OID_OF(int i) { return OID(kIdBase + i); }

bool same_order(const std::optional<Order> &a, const std::optional<Order> &b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->price == b->price && a->orderId == b->orderId && a->quantity == b->quantity &&
           a->side == b->side;
}

bool same_trade(const Trade &a, const Trade &b) {
    return a.aggressive_id == b.aggressive_id && a.passive_id == b.passive_id &&
           a.price == b.price && a.quantity == b.quantity && a.aggressor_side == b.aggressor_side;
}

} // namespace

class MatchingDifferential : public ::testing::TestWithParam<int> {};

TEST_P(MatchingDifferential, RandomSubmitSequence) {
    const int    seed = GetParam();
    std::mt19937 rng(static_cast<std::uint32_t>(seed));

    MatchingEngine      fast;
    NaiveMatchingEngine slow;
    std::vector<Trade>  captured;
    fast.set_trade_handler([&](const Trade &t) { captured.push_back(t); });

    std::string first_diff;
    const auto  check_state = [&]() {
        if (!first_diff.empty()) return;
        if (fast.book().size() != slow.size()) {
            std::ostringstream os;
            os << "size 不一致 fast=" << fast.book().size() << " slow=" << slow.size();
            first_diff = os.str();
            return;
        }
        if (!same_order(fast.book().best_bid(), slow.best_bid())) {
            first_diff = "best_bid 不一致";
            return;
        }
        if (!same_order(fast.book().best_ask(), slow.best_ask())) {
            first_diff = "best_ask 不一致";
            return;
        }
        for (int i = 0; i < kNumIds; ++i) {
            if (!same_order(fast.book().find(OID_OF(i)), slow.find(OID_OF(i)))) {
                std::ostringstream os;
                os << "find(id=" << kIdBase + i << ") 不一致";
                first_diff = os.str();
                return;
            }
        }
        for (std::int64_t p : kPrices) {
            if (fast.book().quantity_at(Price{p}).get() != slow.quantity_at(Price{p}).get()) {
                std::ostringstream os;
                os << "quantity_at(" << p << ") 不一致";
                first_diff = os.str();
                return;
            }
        }
    };

    const OrderType types[] = {OrderType::Limit, OrderType::Market, OrderType::Ioc,
                               OrderType::Fok, OrderType::PostOnly};

    constexpr int kOps = 400;
    for (int step = 0; step < kOps; ++step) {
        const int i    = static_cast<int>(rng() % kNumIds);
        const int roll = static_cast<int>(rng() % 100);

        if (roll < 72) { // submit
            std::int64_t    price = kPrices[rng() % kPrices.size()];
            std::int64_t    qty   = 1 + static_cast<std::int64_t>(rng() % 9);
            const Side      side  = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderType type  = types[rng() % 5];
            if (rng() % 17 == 0) price = -static_cast<std::int64_t>(rng() % 3); // 偶尔非法价
            if (rng() % 19 == 0) qty = 0;                                       // 偶尔非法量
            const Order o{Price{price}, OID_OF(i), Quantity{qty}, side, type};

            captured.clear();
            SubmitResult a = fast.submit(o);
            SubmitResult b = slow.submit(o);

            if (captured.size() != slow.last_trades().size()) {
                std::ostringstream os;
                os << "seed=" << seed << " step=" << step << " 成交笔数不一致 fast="
                   << captured.size() << " slow=" << slow.last_trades().size() << " type="
                   << int(type) << " side=" << int(side) << " price=" << price << " qty=" << qty;
                ADD_FAILURE() << os.str();
                return;
            }
            for (std::size_t k = 0; k < captured.size(); ++k) {
                if (!same_trade(captured[k], slow.last_trades()[k])) {
                    std::ostringstream os;
                    os << "seed=" << seed << " step=" << step << " 第 " << k << " 笔成交不一致";
                    ADD_FAILURE() << os.str();
                    return;
                }
            }
            if (a.outcome != b.outcome || a.reason != b.reason || a.filled != b.filled ||
                a.resting != b.resting || a.trade_count != b.trade_count) {
                std::ostringstream os;
                os << "seed=" << seed << " step=" << step << " SubmitResult 不一致"
                   << " type=" << int(type) << " side=" << int(side);
                ADD_FAILURE() << os.str();
                return;
            }
        }
        else { // cancel（含未知 id）
            const std::int64_t id = (rng() % 5 == 0) ? (90000 + static_cast<std::int64_t>(rng() % 5))
                                                     : kIdBase + i;
            const bool         a  = fast.cancel(OID(id));
            const bool         b  = slow.cancel(OID(id));
            if (a != b) {
                std::ostringstream os;
                os << "seed=" << seed << " step=" << step << " cancel(" << id << ") 返回值不一致";
                ADD_FAILURE() << os.str();
                return;
            }
        }

        check_state();
        if (!first_diff.empty()) {
            std::ostringstream os;
            os << "seed=" << seed << " step=" << step << "：" << first_diff;
            ADD_FAILURE() << os.str();
            return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, MatchingDifferential, ::testing::Range(1, 41));
