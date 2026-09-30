// W4 验收测试（AI 隔离编写，只作为正确性裁判）。
//
// 内容分四部分：
//   1. MatchingW4.*            确定性单元测试：账户与自成交防护（两种策略）/
//                              引擎事件流 / 大数量边界 / 策略切换。
//   2. MatchingW4.Lifetime*    生命周期与内存/UB 压力：账户+STP+订阅churn，
//                              配合 ASan/LSan/UBSan 抓 use-after-free / 泄漏。
//   3. EventBusW4.*            事件总线直接验收：注册顺序 / 退订 / 订阅号。
//   4. Seeds/MatchingDifferentialW4.*  随机差分对拍：真实引擎 vs NaiveMatchingEngineW4，
//                              逐步比对 SubmitResult / 完整事件流 / 簿状态。
//
// 事件捕获（BusCap::got）跨操作累积：垫单之后统一用 tail(got, n) 断言“本次操作之后”。
// 语义契约见 docs/W4_MATCHING_GUIDE.md 的“判据”一节，两者必须逐字一致。
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
#include "naive_matching_engine_w4.h"

using namespace matching_engine;
using namespace maelstrom;
using maelstrom::test_support::NaiveMatchingEngineW4;

namespace {

OrderId   OID(std::int64_t v) { return OrderId{v}; }
AccountId ACC(std::int64_t v) { return AccountId{v}; }

// Order 字段顺序：{Price, OrderId, Quantity, Side, OrderType, AccountId}
Order mk(std::int64_t id, std::int64_t price, std::int64_t qty, Side side,
         OrderType type = OrderType::Limit, std::int64_t account = 0) {
    return Order{Price{price}, OID(id), Quantity{qty}, side, type, ACC(account)};
}

void expect_order(const std::optional<Order> &got, std::int64_t id, std::int64_t price,
                  std::int64_t qty, Side side) {
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->orderId, OID(id));
    EXPECT_EQ(got->price.get(), price);
    EXPECT_EQ(got->quantity.get(), qty);
    EXPECT_EQ(got->side, side);
}

void expect_result(const SubmitResult &r, OrderOutcome outcome, Quantity filled, Quantity resting,
                   std::size_t trade_count, RejectReason reason = RejectReason::None) {
    EXPECT_EQ(r.outcome, outcome);
    EXPECT_EQ(r.reason, reason);
    EXPECT_EQ(r.filled.get(), filled.get());
    EXPECT_EQ(r.resting.get(), resting.get());
    EXPECT_EQ(r.trade_count, trade_count);
}

// ---- 事件构造与比对 --------------------------------------------------------

Event ev_trade(std::int64_t agg_id, std::int64_t agg_acct, Side side, std::int64_t passive_id,
               std::int64_t price, std::int64_t qty) {
    Event ev{};
    ev.kind            = EventKind::Trade;
    ev.order_id        = OID(agg_id);
    ev.account         = ACC(agg_acct);
    ev.side            = side;
    ev.counterparty_id = OID(passive_id);
    ev.price           = Price{price};
    ev.quantity        = Quantity{qty};
    return ev;
}

Event ev_rested(std::int64_t id, std::int64_t acct, Side side, std::int64_t price,
                std::int64_t qty) {
    Event ev{};
    ev.kind     = EventKind::OrderRested;
    ev.order_id = OID(id);
    ev.account  = ACC(acct);
    ev.side     = side;
    ev.price    = Price{price};
    ev.quantity = Quantity{qty};
    return ev;
}

Event ev_canceled(std::int64_t id, std::int64_t acct, Side side, std::int64_t qty) {
    Event ev{};
    ev.kind     = EventKind::OrderCanceled;
    ev.order_id = OID(id);
    ev.account  = ACC(acct);
    ev.side     = side;
    ev.quantity = Quantity{qty};
    return ev;
}

Event ev_rejected(std::int64_t id, std::int64_t acct, Side side, RejectReason reason) {
    Event ev{};
    ev.kind     = EventKind::OrderRejected;
    ev.order_id = OID(id);
    ev.account  = ACC(acct);
    ev.side     = side;
    ev.reason   = reason;
    return ev;
}

void expect_same_event(const Event &g, const Event &w) {
    EXPECT_EQ(g.kind, w.kind);
    EXPECT_EQ(g.order_id.get(), w.order_id.get());
    EXPECT_EQ(g.account.get(), w.account.get());
    EXPECT_EQ(g.side, w.side);
    EXPECT_EQ(g.counterparty_id.get(), w.counterparty_id.get());
    EXPECT_EQ(g.price.get(), w.price.get());
    EXPECT_EQ(g.quantity.get(), w.quantity.get());
    EXPECT_EQ(g.reason, w.reason);
}

void expect_events(const std::vector<Event> &got, const std::vector<Event> &want) {
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        SCOPED_TRACE(testing::Message() << "event[" << i << "]");
        expect_same_event(got[i], want[i]);
    }
}

std::vector<Event> tail(const std::vector<Event> &all, std::size_t from) {
    return std::vector<Event>(all.begin() + static_cast<std::ptrdiff_t>(from), all.end());
}

// 引擎 + 事件捕获。got 跨操作累积，垫单后用 tail(got, n) 取“本次操作之后”。
struct BusCap {
    std::vector<Event> got;
    MatchingEngine     eng;
    SubscriptionId     sub = 0;
    BusCap() { sub = eng.subscribe([this](const Event &e) { got.push_back(e); }); }
};

} // namespace

// ===========================================================================
// 1. 自成交防护（Self-Match Prevention）
// ===========================================================================

TEST(MatchingW4, StpCancelNewestBlocksAndCancelsRemainder) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult r = c.eng.submit(mk(2, 100, 4, Side::Buy, OrderType::Limit, 7));
    expect_result(r, OrderOutcome::Cancelled, Quantity{0}, Quantity{0}, 0);
    expect_order(c.eng.book().best_ask(), 1, 100, 5, Side::Sell); // 挂单纹丝不动
    EXPECT_EQ(c.eng.book().size(), 1u);
    expect_events(tail(c.got, n), {ev_canceled(2, 7, Side::Buy, 4)});
}

TEST(MatchingW4, StpCancelNewestKeepsEarlierTrades) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 1)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    // 先吃掉别人（acct1）的 5 手，随后撞上自己（acct7）的挂单 -> 余量 5 被砍
    SubmitResult r = c.eng.submit(mk(3, 100, 10, Side::Buy, OrderType::Limit, 7));
    expect_result(r, OrderOutcome::Cancelled, Quantity{5}, Quantity{0}, 1);
    expect_order(c.eng.book().best_ask(), 2, 100, 5, Side::Sell);
    expect_events(tail(c.got, n),
                  {ev_trade(3, 7, Side::Buy, 1, 100, 5), ev_canceled(3, 7, Side::Buy, 5)});
}

TEST(MatchingW4, StpCancelOldestRemovesRestingAndContinues) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 100, 3, Side::Sell, OrderType::Limit, 1)).accepted());
    c.eng.set_self_match_policy(SelfMatchPolicy::CancelOldest);
    const std::size_t n = c.got.size();

    // 撞上自己的挂单 -> 撤掉它，继续吃别人的 3 手，余 3 挂簿
    SubmitResult r = c.eng.submit(mk(3, 100, 6, Side::Buy, OrderType::Limit, 7));
    expect_result(r, OrderOutcome::PartiallyFilled, Quantity{3}, Quantity{3}, 1);
    expect_order(c.eng.book().best_bid(), 3, 100, 3, Side::Buy);
    EXPECT_FALSE(c.eng.book().best_ask().has_value());
    expect_events(tail(c.got, n), {ev_canceled(1, 7, Side::Sell, 5),
                                   ev_trade(3, 7, Side::Buy, 2, 100, 3),
                                   ev_rested(3, 7, Side::Buy, 100, 3)});
}

TEST(MatchingW4, StpCancelOldestChainsThroughMultipleOwn) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(3, 100, 3, Side::Sell, OrderType::Limit, 1)).accepted());
    c.eng.set_self_match_policy(SelfMatchPolicy::CancelOldest);
    const std::size_t n = c.got.size();

    // 同价两笔自己的挂单依次被撤，然后吃别人的
    SubmitResult r = c.eng.submit(mk(4, 100, 20, Side::Buy, OrderType::Limit, 7));
    expect_result(r, OrderOutcome::PartiallyFilled, Quantity{3}, Quantity{17}, 1);
    expect_order(c.eng.book().best_bid(), 4, 100, 17, Side::Buy);
    expect_events(tail(c.got, n),
                  {ev_canceled(1, 7, Side::Sell, 5), ev_canceled(2, 7, Side::Sell, 5),
                   ev_trade(4, 7, Side::Buy, 3, 100, 3), ev_rested(4, 7, Side::Buy, 100, 17)});
}

TEST(MatchingW4, StpTriggersOnlyAtContactNotWholeBook) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 3, Side::Sell, OrderType::Limit, 1)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 102, 4, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    // 自己的挂单在 102（第二档），限价 101 吃不到它：STP 不应提前干预
    SubmitResult r = c.eng.submit(mk(3, 101, 5, Side::Buy, OrderType::Limit, 7));
    expect_result(r, OrderOutcome::PartiallyFilled, Quantity{3}, Quantity{2}, 1);
    expect_order(c.eng.book().best_ask(), 2, 102, 4, Side::Sell); // 自己的挂单还在
    expect_events(tail(c.got, n),
                  {ev_trade(3, 7, Side::Buy, 1, 100, 3), ev_rested(3, 7, Side::Buy, 101, 2)});
}

TEST(MatchingW4, ZeroAccountNeverTriggersStp) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 0)).accepted());
    const std::size_t n = c.got.size();

    // 双方账户都是 0（未设置）：永不触发 STP，正常成交（W3 兼容关键）
    SubmitResult r = c.eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Limit, 0));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
    EXPECT_EQ(c.eng.book().size(), 0u);
    expect_events(tail(c.got, n), {ev_trade(2, 0, Side::Buy, 1, 100, 5)});
}

TEST(MatchingW4, DifferentAccountsTradeNormally) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult r = c.eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Limit, 8));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
    expect_events(tail(c.got, n), {ev_trade(2, 8, Side::Buy, 1, 100, 5)});
}

TEST(MatchingW4, StpCancelNewestWithMarketOrder) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 3, Side::Sell, OrderType::Limit, 1)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 100, 4, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult r = c.eng.submit(mk(3, 0, 10, Side::Buy, OrderType::Market, 7));
    expect_result(r, OrderOutcome::Cancelled, Quantity{3}, Quantity{0}, 1);
    expect_order(c.eng.book().best_ask(), 2, 100, 4, Side::Sell);
    expect_events(tail(c.got, n),
                  {ev_trade(3, 7, Side::Buy, 1, 100, 3), ev_canceled(3, 7, Side::Buy, 7)});
}

TEST(MatchingW4, StpFokPrecheckBlindThenCancelledByNewest) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 3, Side::Sell, OrderType::Limit, 1)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    // FOK 预检不看账户：matchable=8 >= 8 通过；执行时撞上自己的 5 手 -> 部分成交后取消
    SubmitResult r = c.eng.submit(mk(3, 101, 8, Side::Buy, OrderType::Fok, 7));
    expect_result(r, OrderOutcome::Cancelled, Quantity{3}, Quantity{0}, 1);
    expect_events(tail(c.got, n),
                  {ev_trade(3, 7, Side::Buy, 1, 100, 3), ev_canceled(3, 7, Side::Buy, 5)});
}

TEST(MatchingW4, StpPostOnlyPrecheckBlindToAccount) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    // 即便是自己账户的挂单，POST-ONLY 预检照样按交叉拒绝（预检不知道 STP）
    SubmitResult r = c.eng.submit(mk(2, 100, 5, Side::Buy, OrderType::PostOnly, 7));
    expect_result(r, OrderOutcome::Rejected, Quantity{0}, Quantity{0}, 0,
                  RejectReason::PostOnlyWouldCross);
    EXPECT_EQ(c.eng.book().size(), 1u);
    expect_events(tail(c.got, n), {ev_rejected(2, 7, Side::Buy, RejectReason::PostOnlyWouldCross)});
}

TEST(MatchingW4, StpAgainstOwnRestingRemainderCancelNewest) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 4, Side::Sell, OrderType::Limit, 1)).accepted());
    SubmitResult first = c.eng.submit(mk(2, 100, 10, Side::Buy, OrderType::Limit, 7));
    expect_result(first, OrderOutcome::PartiallyFilled, Quantity{4}, Quantity{6}, 1);
    const std::size_t n = c.got.size();

    // 自己此前的余量还挂在买一：对手向提交会撞上它
    SubmitResult r = c.eng.submit(mk(3, 100, 6, Side::Sell, OrderType::Limit, 7));
    expect_result(r, OrderOutcome::Cancelled, Quantity{0}, Quantity{0}, 0);
    expect_order(c.eng.book().best_bid(), 2, 100, 6, Side::Buy);
    expect_events(tail(c.got, n), {ev_canceled(3, 7, Side::Sell, 6)});
}

TEST(MatchingW4, StpAgainstOwnRestingRemainderCancelOldest) {
    BusCap c;
    c.eng.set_self_match_policy(SelfMatchPolicy::CancelOldest);
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 4, Side::Sell, OrderType::Limit, 1)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 100, 10, Side::Buy, OrderType::Limit, 7)).accepted());
    const std::size_t n = c.got.size();

    // 撞上自己的余量 -> 撤掉它（旧单），买盘空 -> 自己作为卖单余量挂卖盘
    SubmitResult r = c.eng.submit(mk(3, 100, 6, Side::Sell, OrderType::Limit, 7));
    expect_result(r, OrderOutcome::Rested, Quantity{0}, Quantity{6}, 0);
    expect_order(c.eng.book().best_ask(), 3, 100, 6, Side::Sell);
    EXPECT_FALSE(c.eng.book().best_bid().has_value());
    expect_events(tail(c.got, n),
                  {ev_canceled(2, 7, Side::Buy, 6), ev_rested(3, 7, Side::Sell, 100, 6)});
}

TEST(MatchingW4, PolicySwitchMidStream) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 7)).accepted());
    const std::size_t n0 = c.got.size();

    SubmitResult a = c.eng.submit(mk(2, 100, 3, Side::Buy, OrderType::Limit, 7));
    expect_result(a, OrderOutcome::Cancelled, Quantity{0}, Quantity{0}, 0); // 默认 CN
    expect_order(c.eng.book().best_ask(), 1, 100, 5, Side::Sell);
    expect_events(tail(c.got, n0), {ev_canceled(2, 7, Side::Buy, 3)});

    c.eng.set_self_match_policy(SelfMatchPolicy::CancelOldest);
    const std::size_t n1 = c.got.size();
    SubmitResult      b  = c.eng.submit(mk(3, 100, 3, Side::Buy, OrderType::Limit, 7));
    expect_result(b, OrderOutcome::Rested, Quantity{0}, Quantity{3}, 0); // CO：撤旧单
    EXPECT_FALSE(c.eng.book().best_ask().has_value());
    expect_events(tail(c.got, n1),
                  {ev_canceled(1, 7, Side::Sell, 5), ev_rested(3, 7, Side::Buy, 100, 3)});
}

TEST(MatchingW4, NegativeAccountIdStillTriggersStp) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, -5)).accepted());
    const std::size_t n = c.got.size();

    // 账户“非 0”即启用 STP，负数账户同属一个主体
    SubmitResult r = c.eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Limit, -5));
    expect_result(r, OrderOutcome::Cancelled, Quantity{0}, Quantity{0}, 0);
    expect_events(tail(c.got, n), {ev_canceled(2, -5, Side::Buy, 5)});
}

// ===========================================================================
// 2. 引擎事件流
// ===========================================================================

TEST(MatchingW4, EventSequenceFullFillEmitsOnlyTrades) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 0)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult r = c.eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Limit, 0));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1);
    expect_events(tail(c.got, n), {ev_trade(2, 0, Side::Buy, 1, 100, 5)}); // 全额成交无终态事件
}

TEST(MatchingW4, EventSequencePartialFillRestsEmitsTradeThenRested) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 4, Side::Sell, OrderType::Limit, 0)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult r = c.eng.submit(mk(2, 100, 10, Side::Buy, OrderType::Limit, 0));
    expect_result(r, OrderOutcome::PartiallyFilled, Quantity{4}, Quantity{6}, 1);
    expect_events(tail(c.got, n),
                  {ev_trade(2, 0, Side::Buy, 1, 100, 4), ev_rested(2, 0, Side::Buy, 100, 6)});
}

TEST(MatchingW4, EventSequenceNoCrossEmitsSingleRested) {
    BusCap c;
    SubmitResult r = c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 0));
    expect_result(r, OrderOutcome::Rested, Quantity{0}, Quantity{5}, 0);
    expect_events(c.got, {ev_rested(1, 0, Side::Sell, 100, 5)});
}

TEST(MatchingW4, EventSequenceIocRemainderCanceled) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 0)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult r = c.eng.submit(mk(2, 100, 8, Side::Buy, OrderType::Ioc, 0));
    expect_result(r, OrderOutcome::Cancelled, Quantity{5}, Quantity{0}, 1);
    expect_events(tail(c.got, n),
                  {ev_trade(2, 0, Side::Buy, 1, 100, 5), ev_canceled(2, 0, Side::Buy, 3)});
}

TEST(MatchingW4, EventSequenceMarketOnEmptyBookCanceled) {
    BusCap c;
    SubmitResult r = c.eng.submit(mk(1, 0, 5, Side::Buy, OrderType::Market, 0));
    expect_result(r, OrderOutcome::Cancelled, Quantity{0}, Quantity{0}, 0);
    EXPECT_TRUE(r.accepted());
    expect_events(c.got, {ev_canceled(1, 0, Side::Buy, 5)});
}

TEST(MatchingW4, EventSequenceRejectedCarriesReason) {
    BusCap c;
    SubmitResult a = c.eng.submit(mk(1, 100, 0, Side::Buy, OrderType::Limit, 0));
    EXPECT_EQ(a.reason, RejectReason::InvalidQuantity);
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 0)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult b = c.eng.submit(mk(1, 101, 5, Side::Buy, OrderType::Limit, 0));
    EXPECT_EQ(b.reason, RejectReason::DuplicateOrderId);
    expect_events(c.got, {ev_rejected(1, 0, Side::Buy, RejectReason::InvalidQuantity),
                          ev_rested(1, 0, Side::Sell, 100, 5),
                          ev_rejected(1, 0, Side::Buy, RejectReason::DuplicateOrderId)});
    expect_events(tail(c.got, n),
                  {ev_rejected(1, 0, Side::Buy, RejectReason::DuplicateOrderId)});
}

TEST(MatchingW4, CancelEmitsSingleCanceledWithQuantity) {
    BusCap c;
    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 0)).accepted());
    const std::size_t n = c.got.size();

    EXPECT_TRUE(c.eng.cancel(OID(1)));
    expect_events(tail(c.got, n), {ev_canceled(1, 0, Side::Sell, 5)});

    const std::size_t m = c.got.size();
    EXPECT_FALSE(c.eng.cancel(OID(1)));  // 已不在簿
    EXPECT_FALSE(c.eng.cancel(OID(99))); // 未知 id
    EXPECT_EQ(c.got.size(), m);          // 失败的 cancel 不发事件
}

TEST(MatchingW4, BusAndTradeHandlerAreIndependent) {
    class TradeLog {
    public:
        std::vector<Trade> trades;
        void               operator()(const Trade &t) { trades.push_back(t); }
    };
    TradeLog log;
    BusCap   c;
    c.eng.set_trade_handler([&](const Trade &t) { log(t); });

    ASSERT_TRUE(c.eng.submit(mk(1, 100, 4, Side::Sell, OrderType::Limit, 0)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 100, 10, Side::Buy, OrderType::Limit, 0)).accepted());

    EXPECT_EQ(log.trades.size(), 1u); // W3 通道：只有成交
    ASSERT_EQ(c.got.size(), 3u);      // 总线通道：垫单挂簿 + 成交 + 余量挂簿
    EXPECT_EQ(c.got[0].kind, EventKind::OrderRested);
    EXPECT_EQ(c.got[1].kind, EventKind::Trade);
    EXPECT_EQ(c.got[2].kind, EventKind::OrderRested);
}

TEST(MatchingW4, UnsubscribeStopsEngineEvents) {
    BusCap              c;
    const SubscriptionId extra = c.eng.subscribe([](const Event &) {});
    EXPECT_TRUE(c.eng.unsubscribe(extra));
    EXPECT_TRUE(c.eng.unsubscribe(c.sub)); // BusCap 自己的订阅

    ASSERT_TRUE(c.eng.submit(mk(1, 100, 5, Side::Sell, OrderType::Limit, 0)).accepted());
    SubmitResult r = c.eng.submit(mk(2, 100, 5, Side::Buy, OrderType::Limit, 0));
    expect_result(r, OrderOutcome::Filled, Quantity{5}, Quantity{0}, 1); // 撮合不受影响
    EXPECT_TRUE(c.got.empty());
    EXPECT_FALSE(c.eng.unsubscribe(extra)); // 重复退订
}

// ===========================================================================
// 3. 大数量边界
// ===========================================================================

TEST(MatchingW4, HugeQuantityFokAcrossLevels) {
    BusCap c;
    const std::int64_t big = 1000000000000LL; // 1e12
    ASSERT_TRUE(c.eng.submit(mk(1, 100, big, Side::Sell, OrderType::Limit, 1)).accepted());
    ASSERT_TRUE(c.eng.submit(mk(2, 101, 2 * big, Side::Sell, OrderType::Limit, 1)).accepted());
    const std::size_t n = c.got.size();

    SubmitResult r = c.eng.submit(mk(3, 101, 3 * big, Side::Buy, OrderType::Fok, 2));
    expect_result(r, OrderOutcome::Filled, Quantity{3 * big}, Quantity{0}, 2);
    EXPECT_EQ(c.eng.book().size(), 0u);
    expect_events(tail(c.got, n), {ev_trade(3, 2, Side::Buy, 1, 100, big),
                                   ev_trade(3, 2, Side::Buy, 2, 101, 2 * big)});
}

TEST(MatchingW4, HugeQuantityRestAndCancel) {
    BusCap c;
    const std::int64_t huge = 4000000000000000LL; // 4e15，远超 int32 但安全
    SubmitResult       r    = c.eng.submit(mk(1, 100, huge, Side::Sell, OrderType::Limit, 0));
    expect_result(r, OrderOutcome::Rested, Quantity{0}, Quantity{huge}, 0);
    EXPECT_EQ(c.eng.book().quantity_at(Price{100}).get(), huge);
    ASSERT_EQ(c.got.size(), 1u);
    expect_same_event(c.got[0], ev_rested(1, 0, Side::Sell, 100, huge));

    const std::size_t n = c.got.size();
    EXPECT_TRUE(c.eng.cancel(OID(1)));
    EXPECT_EQ(c.eng.book().size(), 0u);
    expect_events(tail(c.got, n), {ev_canceled(1, 0, Side::Sell, huge)});
}

// ===========================================================================
// 4. 生命周期 / 内存 / UB 压力（配合 ASan/LSan/UBSan 才有意义）
// ===========================================================================

TEST(MatchingW4, LifetimeStressAccountsStpPolicyChurn) {
    std::mt19937                rng(20260930u);
    MatchingEngine              eng;
    std::vector<SubscriptionId> subs;

    std::int64_t id = 1;
    for (int op = 0; op < 400; ++op) {
        if (op % 8 == 0) { // 订阅/退订 churn：压力在 EventBus 的增删上
            if (!subs.empty() && rng() % 2 == 0) {
                const std::size_t k = static_cast<std::size_t>(rng()) % subs.size();
                EXPECT_TRUE(eng.unsubscribe(subs[k]));
                subs.erase(subs.begin() + static_cast<std::ptrdiff_t>(k));
            }
            else {
                subs.push_back(eng.subscribe([](const Event &) {}));
            }
        }
        eng.set_self_match_policy((rng() % 2 == 0) ? SelfMatchPolicy::CancelNewest
                                                   : SelfMatchPolicy::CancelOldest);
        const std::int64_t account = static_cast<std::int64_t>(rng() % 3);
        const std::int64_t price   = 95 + static_cast<std::int64_t>(rng() % 12);
        const std::int64_t qty     = 1 + static_cast<std::int64_t>(rng() % 20);
        const Side         side    = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
        const OrderType    types[] = {OrderType::Limit, OrderType::Market, OrderType::Ioc,
                                      OrderType::Fok, OrderType::PostOnly};
        const OrderType    type    = types[rng() % 5];
        [[maybe_unused]] const SubmitResult r =
            eng.submit(mk(id++, price, qty, side, type, account));
        EXPECT_GE(eng.book().size(), 0u);
    }
    for (std::int64_t i = 1; i < id; ++i) {
        eng.cancel(OID(i));
    }
    EXPECT_EQ(eng.book().size(), 0u);
    for (SubscriptionId s : subs) {
        EXPECT_TRUE(eng.unsubscribe(s));
    }
    EXPECT_FALSE(eng.unsubscribe(1)); // 全部退订后，任何 id 都不存在
}

TEST(MatchingW4, LifetimeDeepFifoSamePriceAccounts) {
    MatchingEngine eng;
    eng.set_self_match_policy(SelfMatchPolicy::CancelOldest);
    constexpr int kOrders = 300;
    for (int i = 0; i < kOrders; ++i) {
        const std::int64_t acct = (i % 2 == 0) ? 0 : 1;
        ASSERT_TRUE(eng.submit(mk(i + 1, 100, 1, Side::Sell, OrderType::Limit, acct)).accepted());
    }
    EXPECT_EQ(eng.book().size(), static_cast<std::size_t>(kOrders));

    std::vector<Event>   got;
    const SubscriptionId sub = eng.subscribe([&](const Event &e) { got.push_back(e); });
    (void)sub;
    // acct1 的买单：撞上同账户挂单逐一撤掉，acct0 挂单被吃；
    // 第 150 笔成交（id=299）即吃满退出——队尾自己的 id=300 不受影响（“吃满即停”）
    SubmitResult r = eng.submit(mk(999999, 100, kOrders / 2, Side::Buy, OrderType::Limit, 1));
    expect_result(r, OrderOutcome::Filled, Quantity{kOrders / 2}, Quantity{0}, kOrders / 2);
    constexpr std::size_t kEvents = static_cast<std::size_t>(kOrders) - 1; // 150 成交 + 149 撤销
    ASSERT_EQ(got.size(), kEvents);
    for (std::size_t i = 0; i < kEvents; ++i) {
        SCOPED_TRACE(testing::Message() << "event[" << i << "]");
        if (i % 2 == 0) { // 先成交 acct0（队头 id=1,3,5,...），再撤 acct1（id=2,4,...）
            EXPECT_EQ(got[i].kind, EventKind::Trade);
            EXPECT_EQ(got[i].counterparty_id.get(), static_cast<std::int64_t>(i + 1));
        }
        else {
            EXPECT_EQ(got[i].kind, EventKind::OrderCanceled);
            EXPECT_EQ(got[i].order_id.get(), static_cast<std::int64_t>(i + 1));
        }
    }
    expect_order(eng.book().best_ask(), 300, 100, 1, Side::Sell); // 幸存的最后一笔
    EXPECT_EQ(eng.book().size(), 1u);
    EXPECT_TRUE(eng.unsubscribe(sub));
}

TEST(MatchingW4, LifetimeBusChurn) {
    EventBus                   bus;
    std::vector<int>           hits(20, 0);
    std::vector<SubscriptionId> ids;
    for (int k = 0; k < 20; ++k) {
        ids.push_back(bus.subscribe(
            [k, &hits](const Event &) { hits[static_cast<std::size_t>(k)]++; }));
    }
    for (int p = 0; p < 1000; ++p) {
        bus.publish(Event{});
        if (p % 200 == 199) { // 每 200 次发布退订一个最早订阅
            EXPECT_TRUE(bus.unsubscribe(ids[static_cast<std::size_t>(p / 200)]));
        }
    }
    // 订阅 k 在第 200*(k+1) 次发布后被移除（k=0..4），其余全程在
    for (int k = 0; k < 20; ++k) {
        const int expect_hits = (k < 5) ? 200 * (k + 1) : 1000;
        EXPECT_EQ(hits[static_cast<std::size_t>(k)], expect_hits) << "subscriber " << k;
    }
    EXPECT_EQ(bus.subscriber_count(), 15u);
}

// ===========================================================================
// 5. 事件总线直接验收
// ===========================================================================

TEST(EventBusW4, SubscribeDeliversInRegistrationOrder) {
    EventBus         bus;
    std::vector<int> order;
    bus.subscribe([&](const Event &) { order.push_back(1); });
    bus.subscribe([&](const Event &) { order.push_back(2); });
    bus.subscribe([&](const Event &) { order.push_back(3); });
    bus.publish(Event{});
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(EventBusW4, UnsubscribeStopsDelivery) {
    EventBus             bus;
    int                  hits = 0;
    const SubscriptionId id   = bus.subscribe([&](const Event &) { hits++; });
    bus.publish(Event{});
    EXPECT_EQ(hits, 1);
    EXPECT_TRUE(bus.unsubscribe(id));
    bus.publish(Event{});
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST(EventBusW4, UnsubscribeUnknownReturnsFalse) {
    EventBus bus;
    EXPECT_FALSE(bus.unsubscribe(999));
    const SubscriptionId id = bus.subscribe([](const Event &) {});
    EXPECT_TRUE(bus.unsubscribe(id));
    EXPECT_FALSE(bus.unsubscribe(id)); // 二次退订
}

TEST(EventBusW4, SubscriberCountTracks) {
    EventBus bus;
    EXPECT_EQ(bus.subscriber_count(), 0u);
    const SubscriptionId a = bus.subscribe([](const Event &) {});
    EXPECT_EQ(bus.subscriber_count(), 1u);
    const SubscriptionId b = bus.subscribe([](const Event &) {});
    EXPECT_EQ(bus.subscriber_count(), 2u);
    EXPECT_TRUE(bus.unsubscribe(a));
    EXPECT_EQ(bus.subscriber_count(), 1u);
    EXPECT_TRUE(bus.unsubscribe(b));
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST(EventBusW4, PublishWithNoSubscribersIsNoop) {
    EventBus bus;
    bus.publish(Event{}); // 不崩溃、无副作用
    bus.publish(Event{});
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST(EventBusW4, SubscriptionIdsStartAtOneAndNeverReuse) {
    EventBus bus;
    EXPECT_EQ(bus.subscribe([](const Event &) {}), SubscriptionId{1});
    const SubscriptionId b = bus.subscribe([](const Event &) {});
    EXPECT_EQ(b, SubscriptionId{2});
    EXPECT_TRUE(bus.unsubscribe(b));
    EXPECT_EQ(bus.subscribe([](const Event &) {}), SubscriptionId{3}); // 不复用 2
}

TEST(EventBusW4, RemovedMiddleKeepsOthersOrder) {
    EventBus         bus;
    std::vector<int> order;
    const SubscriptionId a = bus.subscribe([&](const Event &) { order.push_back(1); });
    const SubscriptionId b = bus.subscribe([&](const Event &) { order.push_back(2); });
    const SubscriptionId c = bus.subscribe([&](const Event &) { order.push_back(3); });
    (void)c;
    EXPECT_TRUE(bus.unsubscribe(b));
    (void)a;
    bus.publish(Event{});
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 3);
}

TEST(EventBusW4, EventDefaultsAreZeroed) {
    Event ev{};
    EXPECT_EQ(ev.order_id.get(), 0);
    EXPECT_EQ(ev.account.get(), 0);
    EXPECT_EQ(ev.counterparty_id.get(), 0);
    EXPECT_EQ(ev.price.get(), 0);
    EXPECT_EQ(ev.quantity.get(), 0);
    EXPECT_EQ(ev.reason, RejectReason::None);
}

// ===========================================================================
// 6. 差分对拍：MatchingEngine vs NaiveMatchingEngineW4
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
           a->side == b->side && a->account == b->account;
}

bool same_event(const Event &a, const Event &b) {
    return a.kind == b.kind && a.order_id == b.order_id && a.account == b.account &&
           a.side == b.side && a.counterparty_id == b.counterparty_id && a.price == b.price &&
           a.quantity == b.quantity && a.reason == b.reason;
}

} // namespace

class MatchingDifferentialW4 : public ::testing::TestWithParam<int> {};

TEST_P(MatchingDifferentialW4, RandomSubmitSequence) {
    const int    seed = GetParam();
    std::mt19937 rng(static_cast<std::uint32_t>(seed));

    // 奇数种子跑 CancelNewest，偶数种子跑 CancelOldest：两种策略都有充分覆盖
    const SelfMatchPolicy policy =
        (seed % 2 == 0) ? SelfMatchPolicy::CancelOldest : SelfMatchPolicy::CancelNewest;

    MatchingEngine        fast;
    NaiveMatchingEngineW4 slow;
    fast.set_self_match_policy(policy);
    slow.set_policy(policy);

    std::vector<Event>   captured;
    const SubscriptionId sub = fast.subscribe([&](const Event &e) { captured.push_back(e); });
    (void)sub;

    std::string first_diff;
    const auto  check_state = [&]() {
        if (!first_diff.empty()) return;
        if (fast.book().size() != slow.size()) {
            first_diff = "size 不一致";
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
            std::int64_t       price = kPrices[rng() % kPrices.size()];
            std::int64_t       qty   = 1 + static_cast<std::int64_t>(rng() % 9);
            const Side         side  = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderType    type  = types[rng() % 5];
            const std::int64_t acct  = static_cast<std::int64_t>(rng() % 3); // 0/1/2
            if (rng() % 17 == 0) price = -static_cast<std::int64_t>(rng() % 3); // 偶尔非法价
            if (rng() % 19 == 0) qty = 0;                                       // 偶尔非法量
            const Order o{Price{price}, OID_OF(i), Quantity{qty}, side, type, ACC(acct)};

            captured.clear();
            SubmitResult a = fast.submit(o);
            SubmitResult b = slow.submit(o);

            if (captured.size() != slow.events().size()) {
                std::ostringstream os;
                os << "seed=" << seed << " step=" << step << " 事件数不一致 fast="
                   << captured.size() << " slow=" << slow.events().size() << " policy="
                   << int(policy) << " type=" << int(type) << " side=" << int(side)
                   << " price=" << price << " qty=" << qty << " acct=" << acct;
                ADD_FAILURE() << os.str();
                return;
            }
            for (std::size_t k = 0; k < captured.size(); ++k) {
                if (!same_event(captured[k], slow.events()[k])) {
                    std::ostringstream os;
                    os << "seed=" << seed << " step=" << step << " 第 " << k
                       << " 个事件不一致 fast_kind=" << int(captured[k].kind)
                       << " slow_kind=" << int(slow.events()[k].kind);
                    ADD_FAILURE() << os.str();
                    return;
                }
            }
            if (a.outcome != b.outcome || a.reason != b.reason || a.filled != b.filled ||
                a.resting != b.resting || a.trade_count != b.trade_count) {
                std::ostringstream os;
                os << "seed=" << seed << " step=" << step << " SubmitResult 不一致"
                   << " policy=" << int(policy) << " type=" << int(type) << " side=" << int(side)
                   << " acct=" << acct;
                ADD_FAILURE() << os.str();
                return;
            }
        }
        else { // cancel（含未知 id）
            const std::int64_t id = (rng() % 5 == 0) ? (90000 + static_cast<std::int64_t>(rng() % 5))
                                                     : kIdBase + i;
            const bool a = fast.cancel(OID(id));
            const bool b = slow.cancel(OID(id));
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

INSTANTIATE_TEST_SUITE_P(Seeds, MatchingDifferentialW4, ::testing::Range(1, 41));
