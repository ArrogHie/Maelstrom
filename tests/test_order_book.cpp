#include <gtest/gtest.h>

#include "Maelstrom/order_book.h"

using namespace maelstrom;

TEST(OrderBook, SmokeAddAndSize) {
    OrderBook book;
    EXPECT_EQ(book.size(), 0u);
    book.add(Order{Price{1}, OrderId{100}, Quantity{10}, Side::Buy});
    EXPECT_EQ(book.size(), 1u);
    EXPECT_TRUE(book.best_bid().has_value());
}