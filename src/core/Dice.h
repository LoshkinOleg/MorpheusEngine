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
