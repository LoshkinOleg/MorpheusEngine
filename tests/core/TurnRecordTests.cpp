#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Errors.h"
#include "core/TurnRecord.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("CheckOutcome round-trips through JSON", "[turnrecord]")
{
    CheckOutcome outcome;
    outcome.stat = "agility";
    outcome.difficulty = "hard";
    outcome.stakes = "fall vs reach the hatch";
    outcome.result.rolls = {9, 5};
    outcome.result.statValue = 3;
    outcome.result.total = 17;
    outcome.result.target = 16;
    outcome.result.success = true;

    const CheckOutcome back = CheckOutcomeFromJson(CheckOutcomeToJson(outcome));
    CHECK(back.stat == "agility");
    CHECK(back.difficulty == "hard");
    CHECK(back.stakes == "fall vs reach the hatch");
    CHECK(back.result.rolls == std::vector<int>{9, 5});
    CHECK(back.result.statValue == 3);
    CHECK(back.result.total == 17);
    CHECK(back.result.target == 16);
    CHECK(back.result.success);
}

TEST_CASE("Applied and rejected changes round-trip through JSON", "[turnrecord]")
{
    const AppliedChange applied{Change{ChangeOp::Add, "player.health", -3, "fell"}, "clamped to 0"};
    const AppliedChange appliedBack = AppliedChangeFromJson(AppliedChangeToJson(applied));
    CHECK(appliedBack.change.op == ChangeOp::Add);
    CHECK(appliedBack.change.path == "player.health");
    CHECK(appliedBack.change.value == -3);
    CHECK(appliedBack.change.reason == "fell");
    CHECK(appliedBack.note == "clamped to 0");

    const RejectedChange rejected{nlohmann::json{{"op", "explode"}}, "unknown op"};
    const RejectedChange rejectedBack = RejectedChangeFromJson(RejectedChangeToJson(rejected));
    CHECK(rejectedBack.raw == nlohmann::json{{"op", "explode"}});
    CHECK(rejectedBack.reason == "unknown op");
}

TEST_CASE("Malformed stored turn data is a DataError", "[turnrecord]")
{
    CHECK_THROWS_AS(CheckOutcomeFromJson(nlohmann::json::array()), DataError);
    CHECK_THROWS_WITH(CheckOutcomeFromJson(nlohmann::json::parse(
                          R"({"stat":"a","difficulty":"b","stakes":"c","rolls":[7],"stat_value":0,"total":7,"target":8})")),
                      ContainsSubstring("success"));
    CHECK_THROWS_AS(AppliedChangeFromJson(nlohmann::json::parse(R"({"change":{"path":"x"},"note":""})")), DataError);
    CHECK_THROWS_AS(RejectedChangeFromJson(nlohmann::json::parse(R"({"raw":1})")), DataError);
}
