#include <catch2/catch_test_macros.hpp>

#include <set>

#include "core/Dice.h"
#include "core/Errors.h"

using namespace morpheus::core;

TEST_CASE("SplitMix64 matches its reference output", "[dice]")
{
    SplitMix64 rng(0);
    CHECK(rng.Next() == 0xe220a8397b1dcdafULL);
}

TEST_CASE("DiceRule parses NdS and NdS+stat", "[dice]")
{
    const DiceRule d20 = DiceRule::Parse("1d20+stat");
    CHECK(d20.count == 1);
    CHECK(d20.sides == 20);
    CHECK(d20.addStat);

    const DiceRule pool = DiceRule::Parse(" 3d6 ");
    CHECK(pool.count == 3);
    CHECK(pool.sides == 6);
    CHECK_FALSE(pool.addStat);

    CHECK(DiceRule::Parse("2D10 + STAT").addStat);
}

TEST_CASE("DiceRule rejects malformed expressions", "[dice]")
{
    for (const char* bad : {"", "d20", "1d", "1x20", "0d6", "21d6", "1d1", "1d20+luck", "1d20+stat+stat"}) {
        INFO(bad);
        CHECK_THROWS_AS(DiceRule::Parse(bad), DataError);
    }
}

TEST_CASE("RollCheck is deterministic per seed and adds the stat", "[dice]")
{
    const DiceRule rule = DiceRule::Parse("2d6+stat");
    const CheckResult first = RollCheck(rule, 3, 10, CheckSeed(42, 1, 0));
    const CheckResult again = RollCheck(rule, 3, 10, CheckSeed(42, 1, 0));

    CHECK(first.rolls == again.rolls);
    REQUIRE(first.rolls.size() == 2);
    CHECK(first.statValue == 3);
    CHECK(first.total == first.rolls[0] + first.rolls[1] + 3);
    CHECK(first.target == 10);
    CHECK(first.success == (first.total >= 10));
}

TEST_CASE("CheckSeed differs across runs, turns and checks", "[dice]")
{
    CHECK(CheckSeed(42, 1, 0) != CheckSeed(42, 2, 0));
    CHECK(CheckSeed(42, 1, 0) != CheckSeed(42, 1, 1));
    CHECK(CheckSeed(42, 1, 0) != CheckSeed(43, 1, 0));
    CHECK_THROWS_AS(CheckSeed(42, 0, 0), ContractError);
}

TEST_CASE("RollCheck covers every face and nothing else", "[dice]")
{
    const DiceRule d6 = DiceRule::Parse("1d6");
    std::set<int> faces;
    for (int turn = 1; turn <= 600; ++turn) {
        const CheckResult result = RollCheck(d6, 5, 1, CheckSeed(7, turn, 0));
        faces.insert(result.rolls.at(0));
        CHECK(result.statValue == 0); // "1d6" does not add the stat.
    }
    CHECK(faces == std::set<int>{1, 2, 3, 4, 5, 6});
}
