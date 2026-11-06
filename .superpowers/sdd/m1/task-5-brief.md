### Task 5: Dice

**Files:**
- Create: `src/core/Dice.h`, `src/core/Dice.cpp`, `tests/core/DiceTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataError`, `ContractError`, `MORPHEUS_REQUIRE` (Task 1).
- Produces (namespace `morpheus::core`):
  - `class SplitMix64 { explicit SplitMix64(std::uint64_t); std::uint64_t Next(); }`
  - `struct DiceRule { int count; int sides; bool addStat; static DiceRule Parse(std::string_view); }`
  - `struct CheckResult { std::vector<int> rolls; int statValue; int total; int target; bool success; }`
  - `std::uint64_t CheckSeed(std::uint64_t runSeed, int turn, int checkIndex)`
  - `CheckResult RollCheck(const DiceRule&, int statValue, int target, std::uint64_t seed)`
- Design decision recorded here: rolls are derived from `(runSeed, turn, checkIndex)`, so regenerating a turn reproduces its rolls. Regenerate changes the prose, not the dice, which also prevents re-rolling by regenerating.

- [ ] **Step 1: Write the failing test `tests/core/DiceTests.cpp`**

```cpp
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
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Dice.h'`.

- [ ] **Step 3: Implement `src/core/Dice.h`**

```cpp
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace morpheus::core {

// SplitMix64: small, fast, well-mixed 64-bit generator. Chosen over std:: engines and distributions because its
// output is identical on every compiler and standard library, which replays rely on.
class SplitMix64 {
private:
    std::uint64_t _state = 0;

public:
    explicit SplitMix64(std::uint64_t seed);

    std::uint64_t Next();
};

// The world's roll expression from world.json: "NdS" or "NdS+stat" (case and spaces ignored).
struct DiceRule {
    int count = 1;
    int sides = 20;
    bool addStat = false;

    // Throws DataError unless the expression has that shape with 1 <= N <= 20 and 2 <= S <= 1000.
    static DiceRule Parse(std::string_view expression);
};

struct CheckResult {
    std::vector<int> rolls = {};
    int statValue = 0; // 0 when the rule does not add a stat.
    int total = 0;
    int target = 0;
    bool success = false; // total >= target
};

// Seed for one check. The same (runSeed, turn, checkIndex) always gives the same seed, so a replayed or
// regenerated turn reproduces its rolls. Throws ContractError if turn < 1 or checkIndex < 0.
std::uint64_t CheckSeed(std::uint64_t runSeed, int turn, int checkIndex);

// Rolls rule.count dice with rule.sides faces, adds statValue when rule.addStat, and compares with target.
CheckResult RollCheck(const DiceRule& rule, int statValue, int target, std::uint64_t seed);

} // namespace morpheus::core
```

- [ ] **Step 4: Implement `src/core/Dice.cpp`, and add `Dice.cpp` to `src/core/CMakeLists.txt` after `Changes.cpp`**

```cpp
#include "core/Dice.h"

#include <cctype>
#include <charconv>
#include <format>
#include <string>

#include "core/Errors.h"

namespace morpheus::core {

SplitMix64::SplitMix64(std::uint64_t seed)
    : _state(seed)
{
}

std::uint64_t SplitMix64::Next()
{
    // Reference SplitMix64 (Steele, Lea, Flood 2014): golden-ratio increment followed by two xor-shift-multiply rounds.
    std::uint64_t z = (_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

DiceRule DiceRule::Parse(std::string_view expression)
{
    const std::string where = std::format("dice expression '{}'", expression);

    // Normalize: drop spaces and lower-case, so "2D10 + STAT" reads as "2d10+stat".
    std::string compact;
    for (const char c : expression) {
        if (c != ' ') {
            compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }

    DiceRule rule;
    std::string_view rest = compact;
    if (rest.ends_with("+stat")) {
        rule.addStat = true;
        rest.remove_suffix(5);
    }

    const std::size_t d = rest.find('d');
    if (d == std::string_view::npos || d == 0 || d + 1 == rest.size()) {
        throw DataError(where + ": expected NdS or NdS+stat");
    }

    // Both numbers must be plain decimal integers that consume their whole segment.
    auto parseNumber = [&where](std::string_view digits) {
        int value = 0;
        const auto [pEnd, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc() || pEnd != digits.data() + digits.size()) {
            throw DataError(where + ": expected NdS or NdS+stat");
        }
        return value;
    };
    rule.count = parseNumber(rest.substr(0, d));
    rule.sides = parseNumber(rest.substr(d + 1));

    if (rule.count < 1 || rule.count > 20) {
        throw DataError(where + ": dice count must be 1..20");
    }
    if (rule.sides < 2 || rule.sides > 1000) {
        throw DataError(where + ": die sides must be 2..1000");
    }
    return rule;
}

std::uint64_t CheckSeed(std::uint64_t runSeed, int turn, int checkIndex)
{
    MORPHEUS_REQUIRE(turn >= 1 && checkIndex >= 0, "CheckSeed: turn must be >= 1 and checkIndex >= 0");

    // Turn in the high half, check index in the low half, then one SplitMix64 step to spread the bits.
    SplitMix64 mixer(runSeed ^ (static_cast<std::uint64_t>(turn) << 32) ^ static_cast<std::uint64_t>(checkIndex));
    return mixer.Next();
}

CheckResult RollCheck(const DiceRule& rule, int statValue, int target, std::uint64_t seed)
{
    SplitMix64 rng(seed);
    CheckResult result;
    result.statValue = rule.addStat ? statValue : 0;
    result.target = target;
    result.total = result.statValue;

    // Modulo bias is below 1e-16 for these die sizes, irrelevant for gameplay.
    for (int i = 0; i < rule.count; ++i) {
        const int roll = 1 + static_cast<int>(rng.Next() % static_cast<std::uint64_t>(rule.sides));
        result.rolls.push_back(roll);
        result.total += roll;
    }
    result.success = result.total >= target;
    return result;
}

} // namespace morpheus::core
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/core tests/core/DiceTests.cpp
git commit -m "Add deterministic dice and check resolution" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

