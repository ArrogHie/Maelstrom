#pragma once
#include <cstdint>

namespace maelstrom {
template <typename Tag, typename T>
struct StrongType { // 强类型，防止类型传递混乱
    T value;
    constexpr explicit StrongType(T v = T{}) noexcept : value(v) {}
    [[nodiscard]] constexpr T get() const noexcept { return value; }

    constexpr StrongType operator+(const StrongType &other) const noexcept { return StrongType{value + other.get()}; }
    constexpr StrongType operator-(const StrongType &other) const noexcept { return StrongType{value - other.get()}; }

    constexpr bool operator==(const StrongType &other) const noexcept { return value == other.value; }
    constexpr bool operator!=(const StrongType &other) const noexcept { return value != other.value; }
    constexpr bool operator<(const StrongType &other) const noexcept { return value < other.value; }
    constexpr bool operator<=(const StrongType &other) const noexcept { return value <= other.value; }
    constexpr bool operator>(const StrongType &other) const noexcept { return value > other.value; }
    constexpr bool operator>=(const StrongType &other) const noexcept { return value >= other.value; }
};

struct PriceTag {};
struct QuantityTag {};
struct OrderIdTag {};
struct AccountIdTag {};

using Price          = StrongType<PriceTag, std::int64_t>;
using Quantity       = StrongType<QuantityTag, std::int64_t>;
using OrderId        = StrongType<OrderIdTag, std::int64_t>;
using AccountId      = StrongType<AccountIdTag, std::int64_t>;
using SubscriptionId = std::int64_t;

} // namespace maelstrom