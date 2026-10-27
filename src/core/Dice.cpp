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

    // Mix the run seed first, so run seeds that differ only in high bits cannot cancel against the turn bits.
    const std::uint64_t base = SplitMix64(runSeed).Next();

    // Then turn in the high half, check index in the low half, and one more SplitMix64 step to spread the bits.
    SplitMix64 mixer(base ^ ((static_cast<std::uint64_t>(turn) << 32) | static_cast<std::uint64_t>(checkIndex)));
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
