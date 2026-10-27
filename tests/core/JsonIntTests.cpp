#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "core/JsonInt.h"

using morpheus::core::JsonIntInRange;

TEST_CASE("JsonIntInRange accepts the exact boundaries", "[jsonint]")
{
    CHECK(JsonIntInRange(nlohmann::json(3), 3, 9) == 3);
    CHECK(JsonIntInRange(nlohmann::json(9), 3, 9) == 9);
    CHECK(JsonIntInRange(nlohmann::json(5), 3, 9) == 5);
}

TEST_CASE("JsonIntInRange rejects values just outside the bounds", "[jsonint]")
{
    CHECK_FALSE(JsonIntInRange(nlohmann::json(2), 3, 9).has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json(10), 3, 9).has_value());
}

TEST_CASE("JsonIntInRange handles negative values and bounds", "[jsonint]")
{
    CHECK(JsonIntInRange(nlohmann::json(-5), -5, 5) == -5);
    CHECK(JsonIntInRange(nlohmann::json(-6), -5, 5).has_value() == false);
    CHECK(JsonIntInRange(nlohmann::json(-1), 0, 5).has_value() == false);
    CHECK(JsonIntInRange(nlohmann::json(-1), -10, -1) == -1);
}

TEST_CASE("JsonIntInRange works with INT_MIN and INT_MAX as bounds", "[jsonint]")
{
    constexpr long long INT_LO = std::numeric_limits<int>::min();
    constexpr long long INT_HI = std::numeric_limits<int>::max();

    CHECK(JsonIntInRange(nlohmann::json(INT_LO), INT_LO, INT_HI) == INT_LO);
    CHECK(JsonIntInRange(nlohmann::json(INT_HI), INT_LO, INT_HI) == INT_HI);
    CHECK_FALSE(JsonIntInRange(nlohmann::json(INT_LO - 1), INT_LO, INT_HI).has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json(INT_HI + 1), INT_LO, INT_HI).has_value());
    // The same values parsed from text arrive as unsigned (positive) or signed (negative) numbers.
    CHECK(JsonIntInRange(nlohmann::json::parse("2147483647"), INT_LO, INT_HI) == INT_HI);
    CHECK(JsonIntInRange(nlohmann::json::parse("-2147483648"), INT_LO, INT_HI) == INT_LO);
}

TEST_CASE("JsonIntInRange never wraps a huge unsigned value", "[jsonint]")
{
    constexpr long long INT_LO = std::numeric_limits<int>::min();
    constexpr long long INT_HI = std::numeric_limits<int>::max();

    CHECK_FALSE(JsonIntInRange(nlohmann::json(std::numeric_limits<std::uint64_t>::max()), INT_LO, INT_HI).has_value());
    // Even with the widest possible bounds, uint64 max would wrap to -1 as a long long.
    CHECK_FALSE(JsonIntInRange(nlohmann::json(std::numeric_limits<std::uint64_t>::max()),
                               std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max())
                    .has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json::parse("18446744073709551615"), -10, 10).has_value());
    // Just above LLONG_MAX, as unsigned.
    CHECK_FALSE(JsonIntInRange(nlohmann::json(std::uint64_t{9223372036854775808ULL}), INT_LO, INT_HI).has_value());
    // LLONG_MAX itself is a valid long long when the bounds allow it.
    CHECK(JsonIntInRange(nlohmann::json(std::numeric_limits<long long>::max()), 0,
                         std::numeric_limits<long long>::max()) == std::numeric_limits<long long>::max());
}

TEST_CASE("JsonIntInRange rejects non-integers", "[jsonint]")
{
    CHECK_FALSE(JsonIntInRange(nlohmann::json(2.5), 0, 10).has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json(5.0), 0, 10).has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json(true), 0, 10).has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json("5"), 0, 10).has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json(nullptr), 0, 10).has_value());
    CHECK_FALSE(JsonIntInRange(nlohmann::json::array({5}), 0, 10).has_value());
}
