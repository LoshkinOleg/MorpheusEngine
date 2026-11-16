#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/World.h"

using namespace morpheus::core;
using namespace nlohmann::literals;

namespace {

// The observation the conditions read: a quest stage, a name, a flag and an object.
const nlohmann::json OBSERVATION = R"({
    "quests": {"MG03": 55, "MG04": 30, "title": "Gone"},
    "weather": "rain",
    "open": true,
    "gate": {"state": "shut", "locks": 2}
})"_json;

LoreCondition Condition(const char* pointer, std::optional<double> min, std::optional<double> max,
                        std::optional<nlohmann::json> equals = std::nullopt)
{
    LoreCondition condition;
    condition.pointer = pointer;
    condition.min = min;
    condition.max = max;
    condition.equals = std::move(equals);
    return condition;
}

} // namespace

TEST_CASE("LoreCondition min holds at and above the bound, and not below it", "[lore]")
{
    const LoreCondition atLeast40 = Condition("/quests/MG03", 40, std::nullopt);
    CHECK(atLeast40.Holds(R"({"quests": {"MG03": 40}})"_json));
    CHECK(atLeast40.Holds(OBSERVATION)); // 55
    CHECK_FALSE(atLeast40.Holds(R"({"quests": {"MG03": 30}})"_json));
    CHECK_FALSE(atLeast40.Holds(R"({"quests": {"MG03": 39.5}})"_json));
}

TEST_CASE("LoreCondition max is inclusive, and min and max together make a range", "[lore]")
{
    const LoreCondition atMost40 = Condition("/quests/MG03", std::nullopt, 40);
    CHECK(atMost40.Holds(R"({"quests": {"MG03": 40}})"_json));
    CHECK(atMost40.Holds(R"({"quests": {"MG03": 30}})"_json));
    CHECK_FALSE(atMost40.Holds(OBSERVATION)); // 55

    const LoreCondition between = Condition("/quests/MG03", 40, 50);
    CHECK(between.Holds(R"({"quests": {"MG03": 40}})"_json));
    CHECK(between.Holds(R"({"quests": {"MG03": 50}})"_json));
    CHECK_FALSE(between.Holds(R"({"quests": {"MG03": 39}})"_json));
    CHECK_FALSE(between.Holds(R"({"quests": {"MG03": 51}})"_json));
}

TEST_CASE("LoreCondition equals compares JSON values: a string, a number and an object", "[lore]")
{
    CHECK(Condition("/weather", std::nullopt, std::nullopt, nlohmann::json("rain")).Holds(OBSERVATION));
    CHECK_FALSE(Condition("/weather", std::nullopt, std::nullopt, nlohmann::json("snow")).Holds(OBSERVATION));
    CHECK(Condition("/quests/MG03", std::nullopt, std::nullopt, nlohmann::json(55)).Holds(OBSERVATION));
    CHECK_FALSE(Condition("/quests/MG03", std::nullopt, std::nullopt, nlohmann::json(56)).Holds(OBSERVATION));
    CHECK(Condition("/gate", std::nullopt, std::nullopt, R"({"locks": 2, "state": "shut"})"_json).Holds(OBSERVATION));
    CHECK_FALSE(Condition("/gate", std::nullopt, std::nullopt, R"({"state": "shut"})"_json).Holds(OBSERVATION));
    CHECK(Condition("/open", std::nullopt, std::nullopt, nlohmann::json(true)).Holds(OBSERVATION));
}

TEST_CASE("LoreCondition does not hold on a missing value or a value of another type", "[lore]")
{
    // A missing pointer.
    CHECK_FALSE(Condition("/quests/MG99", 0, std::nullopt).Holds(OBSERVATION));
    CHECK_FALSE(Condition("/nowhere/deep", std::nullopt, std::nullopt, nlohmann::json(1)).Holds(OBSERVATION));
    // A string, or a boolean, where min or max is given; a number where equals wants a string.
    CHECK_FALSE(Condition("/quests/title", 0, std::nullopt).Holds(OBSERVATION));
    CHECK_FALSE(Condition("/weather", std::nullopt, 10).Holds(OBSERVATION));
    CHECK_FALSE(Condition("/open", 0, std::nullopt).Holds(OBSERVATION));
    CHECK_FALSE(Condition("/weather", std::nullopt, std::nullopt, nlohmann::json(5)).Holds(OBSERVATION));
    // Every comparison must hold: min met, equals not.
    CHECK_FALSE(Condition("/quests/MG03", 40, std::nullopt, nlohmann::json(56)).Holds(OBSERVATION));
    CHECK(Condition("/quests/MG03", 40, std::nullopt, nlohmann::json(55)).Holds(OBSERVATION));
}

TEST_CASE("LoreCondition: an array index with a leading zero does not hold and does not throw", "[lore]")
{
    // JSON Pointer array indexes have no leading zeros: /items/01 names nothing.
    const nlohmann::json items = R"({"items": [5, 6]})"_json;
    CHECK_FALSE(Condition("/items/01", 0, std::nullopt).Holds(items));
    CHECK_NOTHROW(Condition("/items/01", 0, std::nullopt).Holds(items));
    CHECK(Condition("/items/1", 6, 6).Holds(items));
}

TEST_CASE("LoreCondition with a pointer that is not well-formed is a contract error", "[lore]")
{
    CHECK_THROWS_AS(Condition("quests", 0, std::nullopt).Holds(OBSERVATION), ContractError);
}
