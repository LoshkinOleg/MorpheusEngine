#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <set>
#include <vector>

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

TEST_CASE("CheckSeed mixes the run seed so high-bit differences cannot collide across turns", "[dice]")
{
    // Without mixing, (A, turn 1) and (A ^ (3 << 32), turn 2) xor to the same value.
    const std::uint64_t a = 0x123456789abcdef0ULL;
    CHECK(CheckSeed(a, 1, 0) != CheckSeed(a ^ (3ULL << 32), 2, 0));
    // The same holds for the check index in the low half.
    CHECK(CheckSeed(a, 1, 0) != CheckSeed(a ^ 5ULL, 1, 5));
}

TEST_CASE("RollCheck succeeds exactly when total reaches the target", "[dice]")
{
    const DiceRule rule = DiceRule::Parse("2d6+stat");
    const std::uint64_t seed = CheckSeed(42, 1, 0);
    const long long total = RollCheck(rule, 3, 0, seed).total;

    // Same seed, so the same dice: only the target moves.
    const CheckResult atTarget = RollCheck(rule, 3, static_cast<int>(total), seed);
    CHECK(atTarget.total == total);
    CHECK(atTarget.success);

    const CheckResult justAbove = RollCheck(rule, 3, static_cast<int>(total) + 1, seed);
    CHECK(justAbove.total == total);
    CHECK_FALSE(justAbove.success);
}

TEST_CASE("RollCheck totals do not overflow int with a huge stat", "[dice]")
{
    const DiceRule rule = DiceRule::Parse("1d20+stat");
    const CheckResult result = RollCheck(rule, std::numeric_limits<int>::max(), 10, CheckSeed(42, 1, 0));

    REQUIRE(result.rolls.size() == 1);
    CHECK(result.total == static_cast<long long>(std::numeric_limits<int>::max()) + result.rolls[0]);
    CHECK(result.total > std::numeric_limits<int>::max());
    CHECK(result.success);
}

TEST_CASE("Dice outputs are pinned so replays stay stable", "[dice]")
{
    // These golden values guard replay stability: changing the seed mixing or the roll derivation changes every
    // saved run's checks. They were computed once and cross-checked with an independent SplitMix64 implementation.
    CHECK(CheckSeed(42, 1, 0) == 0x8a5509b0d5741cc5ULL);

    const CheckResult result = RollCheck(DiceRule::Parse("2d6+stat"), 3, 10, CheckSeed(42, 1, 0));
    CHECK(result.rolls == std::vector<int>{1, 5});
    CHECK(result.total == 9);
    CHECK_FALSE(result.success);
}

TEST_CASE("SamplingSeed is stable and separates turns, calls and attempts", "[dice]")
{
    const std::uint32_t resolve = SamplingSeed(42, 3, CallKind::Resolve, 0);
    CHECK(SamplingSeed(42, 3, CallKind::Resolve, 0) == resolve);
    CHECK(SamplingSeed(42, 3, CallKind::Narrate, 0) != resolve);
    CHECK(SamplingSeed(42, 3, CallKind::Resolve, 1) != resolve);
    CHECK(SamplingSeed(42, 4, CallKind::Resolve, 0) != resolve);
    CHECK(SamplingSeed(43, 3, CallKind::Resolve, 0) != resolve);
    CHECK(SamplingSeed(42, 0, CallKind::Summarize, 0) == SamplingSeed(42, 0, CallKind::Summarize, 0));
    CHECK_THROWS_AS(SamplingSeed(42, -1, CallKind::Resolve, 0), ContractError);
    CHECK_THROWS_AS(SamplingSeed(42, 1, CallKind::Resolve, -1), ContractError);
}
