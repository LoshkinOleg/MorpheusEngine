### Task 4: `IntRange`, exact for any `long long` range

**The algorithm.** `IntRange(min, max)` splits the range at zero and writes each side as alternatives:
1. **Sides.** The negative numbers are `"-"` followed by a magnitude from `|min(max, -1)|` to `|min|`; zero is the literal `"0"`; the positive numbers are magnitudes from `max(min, 1)` to `max`. Magnitudes are `unsigned long long`, computed as `0ULL - static_cast<unsigned long long>(v)`, so the magnitude of `LLONG_MIN` (2^63) fits. Since every magnitude is at least 1 and written without a leading zero, no `-0` and no leading zeros can appear.
2. **Bands by digit count.** A magnitude range `lo..hi` (1 <= lo <= hi) is split by digit count `d`: the band `10^(d-1)..10^d - 1`. The lowest band starts at `lo` and is partial unless `lo` is a power of ten; the highest ends at `hi` and is partial unless `hi` is all nines; the bands in between are full. Consecutive full bands share one alternative: `[1-9]` followed by `[0-9]{a,b}` free digits (so `1..999999999` is `[1-9] [0-9]{0,8}`, today's `pos-int`).
3. **One band, digit by digit.** A partial band is a range of digit strings of one length, `low..high`. Past their shared prefix (written once as a literal), the first differing digit `a < b` splits it into: `a` followed by the range `low's rest..99..9`; the free middle `[a+1-b-1]` followed by free digits; and `b` followed by `00..0..high's rest`. When `low`'s rest is all zeros, `a` joins the free middle; when `high`'s rest is all nines, `b` does. The two ends recurse on strings one digit shorter, so the depth is at most 19.

Each alternative is a sequence; a group with more than one alternative is wrapped in parentheses. Exactness follows from the decomposition: the bands partition `lo..hi` by length, and within a band the three parts partition it by the first differing digit. Tests: hand-computed texts (single values, across zero, negative only, several digit counts, partial bottom and top bands, Morpheus's `int` and `pos-int`), a brute force over many ranges with the matcher (every integer in a window around both ends is accepted iff it is in range, and no other spelling is ever accepted), and the whole `long long` range at its edges.

**Files:**
- Modify: `src/grammar/Gbnf.h`, `src/grammar/Gbnf.cpp`
- Test: `tests/grammar/GbnfTests.cpp`

**Interfaces:**
- Consumes: Task 3's builder; `test::GbnfMatcher`.
- Produces: `std::string grammar::IntRange(long long min, long long max);` (`ContractError` when `min > max`).

- [ ] **Step 1: Write the failing tests**

In `tests/grammar/GbnfTests.cpp`, replace

```cpp
#include <string>
#include <vector>
```

with

```cpp
#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>
```

and append:

```cpp
TEST_CASE("IntRange writes each digit count and boundary as alternatives", "[int-range]")
{
    // Morpheus's own ranges: int, pos-int and the negative half of nonzero-int.
    CHECK(grammar::IntRange(-999999999, 999999999) == R"gbnf(("-" [1-9] [0-9]{0,8} | "0" | [1-9] [0-9]{0,8}))gbnf");
    CHECK(grammar::IntRange(1, 999999999) == "[1-9] [0-9]{0,8}");
    CHECK(grammar::IntRange(-999999999, -1) == R"gbnf("-" [1-9] [0-9]{0,8})gbnf");
    // Single values.
    CHECK(grammar::IntRange(0, 0) == R"gbnf("0")gbnf");
    CHECK(grammar::IntRange(7, 7) == R"gbnf("7")gbnf");
    CHECK(grammar::IntRange(-7, -7) == R"gbnf("-" "7")gbnf");
    CHECK(grammar::IntRange(250, 250) == R"gbnf("250")gbnf");
    // Across zero, and within one digit count.
    CHECK(grammar::IntRange(-1, 1) == R"gbnf(("-" "1" | "0" | "1"))gbnf");
    CHECK(grammar::IntRange(0, 9) == R"gbnf(("0" | [1-9]))gbnf");
    CHECK(grammar::IntRange(10, 99) == "[1-9] [0-9]");
    // Several digit counts: a partial top band, a negative side, a partial bottom band.
    CHECK(grammar::IntRange(0, 255) ==
          R"gbnf(("0" | [1-9] [0-9]{0,1} | "1" [0-9]{2} | "2" ([0-4] [0-9] | "5" [0-5])))gbnf");
    CHECK(grammar::IntRange(-15, 123) ==
          R"gbnf(("-" ([1-9] | "1" [0-5]) | "0" | [1-9] [0-9]{0,1} | "1" ([0-1] [0-9] | "2" [0-3])))gbnf");
    CHECK(grammar::IntRange(37, 120) == R"gbnf(("3" [7-9] | [4-9] [0-9] | "1" ([0-1] [0-9] | "2" "0")))gbnf");

    CHECK_THROWS_AS(grammar::IntRange(2, 1), core::ContractError);
}

TEST_CASE("IntRange accepts exactly the integers of its range, each written one way", "[int-range]")
{
    // Every range between two of these ends, and random ranges, each probed around both ends and inside.
    const std::vector<long long> ends = {-1001, -1000, -999, -101, -100, -99, -91, -90, -10, -9, -1, 0, 1,
                                         9,     10,    11,   19,   20,   99,  100, 101, 199, 255, 999, 1000, 1001};
    std::vector<std::pair<long long, long long>> ranges;
    for (std::size_t i = 0; i < ends.size(); ++i) {
        for (std::size_t j = i; j < ends.size(); ++j) {
            ranges.emplace_back(ends[i], ends[j]);
        }
    }
    std::mt19937_64 random(20261114);
    for (int i = 0; i < 200; ++i) {
        const long long low = std::uniform_int_distribution<long long>(-1000000, 1000000)(random);
        ranges.emplace_back(low, low + std::uniform_int_distribution<long long>(0, 100000)(random));
    }

    for (const auto& [low, high] : ranges) {
        const test::GbnfMatcher matcher = MatcherFor(grammar::IntRange(low, high));
        std::vector<long long> probes;
        for (long long delta = -15; delta <= 15; ++delta) {
            probes.push_back(low + delta);
            probes.push_back(high + delta);
        }
        for (long long eighth = 1; eighth < 8; ++eighth) {
            probes.push_back(low + (high - low) * eighth / 8);
        }
        for (const long long value : probes) {
            INFO("IntRange(" << low << ", " << high << ") on " << value);
            CHECK(matcher.Accepts(std::to_string(value)) == (low <= value && value <= high));
        }
        // Whatever the range, no other spelling: a leading zero, "-0", a plus sign, a space, nothing.
        for (const long long value : {low, high}) {
            const std::string digits = std::to_string(value < 0 ? -value : value);
            INFO("IntRange(" << low << ", " << high << ") on other spellings of " << value);
            CHECK_FALSE(matcher.Accepts((value < 0 ? "-0" : "0") + digits));
            CHECK_FALSE(matcher.Accepts("+" + digits));
            CHECK_FALSE(matcher.Accepts(" " + std::to_string(value)));
        }
        CHECK_FALSE(matcher.Accepts("-0"));
        CHECK_FALSE(matcher.Accepts("-"));
        CHECK_FALSE(matcher.Accepts(""));
    }
}

TEST_CASE("IntRange covers the whole long long range exactly", "[int-range]")
{
    constexpr long long LOWEST = std::numeric_limits<long long>::min();
    constexpr long long HIGHEST = std::numeric_limits<long long>::max();

    const test::GbnfMatcher all = MatcherFor(grammar::IntRange(LOWEST, HIGHEST));
    CHECK(all.Accepts("-9223372036854775808"));
    CHECK(all.Accepts("9223372036854775807"));
    CHECK(all.Accepts("0"));
    CHECK(all.Accepts("-1"));
    CHECK(all.Accepts("1000000000000000000"));
    CHECK(all.Accepts("-999999999999999999"));
    CHECK_FALSE(all.Accepts("-9223372036854775809"));
    CHECK_FALSE(all.Accepts("9223372036854775808"));
    CHECK_FALSE(all.Accepts("9999999999999999999"));
    CHECK_FALSE(all.Accepts("10000000000000000000"));
    CHECK_FALSE(all.Accepts("-0"));
    CHECK_FALSE(all.Accepts("01"));

    // A window at each end.
    const test::GbnfMatcher top = MatcherFor(grammar::IntRange(HIGHEST - 30, HIGHEST));
    for (long long value = HIGHEST - 40; value < HIGHEST; ++value) {
        INFO(value);
        CHECK(top.Accepts(std::to_string(value)) == (value >= HIGHEST - 30));
    }
    CHECK(top.Accepts("9223372036854775807"));
    CHECK_FALSE(top.Accepts("9223372036854775808"));
    const test::GbnfMatcher bottom = MatcherFor(grammar::IntRange(LOWEST, LOWEST + 30));
    for (long long value = LOWEST + 1; value < LOWEST + 40; ++value) {
        INFO(value);
        CHECK(bottom.Accepts(std::to_string(value)) == (value <= LOWEST + 30));
    }
    CHECK(bottom.Accepts("-9223372036854775808"));
    CHECK_FALSE(bottom.Accepts("-9223372036854775809"));

    // The lowest value alone.
    const test::GbnfMatcher lowest = MatcherFor(grammar::IntRange(LOWEST, LOWEST));
    CHECK(lowest.Accepts("-9223372036854775808"));
    CHECK_FALSE(lowest.Accepts("-9223372036854775807"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/grammar/GbnfTests.cpp`: `'IntRange': is not a member of 'morpheus::grammar'`.

- [ ] **Step 3: Declare `IntRange`**

In `src/grammar/Gbnf.h`, replace

```cpp
std::string JsonString(int minChars, int maxChars);
```

with

```cpp
std::string JsonString(int minChars, int maxChars);

// The decimal JSON integers from min to max, both included, exactly: no leading zeros, no "-0", no "+". Any range of
// long long with min <= max. Its digit classes are written inline: it needs no shared terminal.
std::string IntRange(long long min, long long max);
```

- [ ] **Step 4: Implement it**

In `src/grammar/Gbnf.cpp`, replace

```cpp
#include <algorithm>
#include <cstddef>
#include <format>
#include <set>
```

with

```cpp
#include <algorithm>
#include <cstddef>
#include <format>
#include <set>
#include <string>
```

replace

```cpp
// A repetition count: "{n}" for exactly n, "{m,n}" for a range.
std::string Count(std::size_t fewest, std::size_t most)
{
    return fewest == most ? std::format("{{{}}}", most) : std::format("{{{},{}}}", fewest, most);
}

} // namespace
```

with

```cpp
// A repetition count: "{n}" for exactly n, "{m,n}" for a range.
std::string Count(std::size_t fewest, std::size_t most)
{
    return fewest == most ? std::format("{{{}}}", most) : std::format("{{{},{}}}", fewest, most);
}

// fewest to most free digits.
std::string AnyDigits(std::size_t fewest, std::size_t most)
{
    return fewest == 1 && most == 1 ? std::string("[0-9]") : "[0-9]" + Count(fewest, most);
}

// The alternatives matching every digit string as long as `low`, from `low` to `high` (equal lengths, low <= high,
// which as digit strings of one length compares as numbers). Leading zeros are allowed here: the callers give a first
// digit of 1 to 9, or a suffix. Recursive: past the shared prefix, the first digit that differs splits the range into
// its low end, a middle of free digits and its high end.
std::vector<std::string> SameLengthAlternatives(const std::string& low, const std::string& high)
{
    if (low == high) {
        return {Literal(low)};
    }
    // A digit from `from` to `to`: a literal for one digit, a class for several.
    auto digitsFrom = [](char from, char to) {
        return from == to ? Literal(std::string(1, from)) : std::format("[{}-{}]", from, to);
    };

    // The first position where the two differ; the digits before it are shared.
    std::size_t split = 0;
    while (low[split] == high[split]) {
        ++split;
    }
    const char a = low[split];
    const char b = high[split];
    const std::size_t rest = low.size() - split - 1; // Digits after the split.
    std::vector<std::string> alternatives;
    if (rest == 0) {
        alternatives.push_back(digitsFrom(a, b));
    } else {
        const std::string lowRest = low.substr(split + 1);
        const std::string highRest = high.substr(split + 1);
        // A low end of `a` then all zeros, or a high end of `b` then all nines, joins the middle's free digits.
        const bool lowFull = lowRest == std::string(rest, '0');
        const bool highFull = highRest == std::string(rest, '9');
        const char first = lowFull ? a : static_cast<char>(a + 1);
        const char last = highFull ? b : static_cast<char>(b - 1);
        if (!lowFull) {
            alternatives.push_back(Literal(std::string(1, a)) + " " +
                                   Alternatives(SameLengthAlternatives(lowRest, std::string(rest, '9'))));
        }
        if (first <= last) {
            alternatives.push_back(digitsFrom(first, last) + " " + AnyDigits(rest, rest));
        }
        if (!highFull) {
            alternatives.push_back(Literal(std::string(1, b)) + " " +
                                   Alternatives(SameLengthAlternatives(std::string(rest, '0'), highRest)));
        }
    }
    // The shared prefix is written once, in front of the group.
    if (split == 0) {
        return alternatives;
    }
    return {Literal(low.substr(0, split)) + " " + Alternatives(alternatives)};
}

// The alternatives matching the decimal numbers from lo to hi (1 <= lo <= hi), without leading zeros. Each digit
// count is a band: the shortest and the longest may be partial, every band between them is full, and neighbouring
// full bands share one alternative, a first digit of 1 to 9 and then free digits.
std::vector<std::string> MagnitudeAlternatives(unsigned long long lo, unsigned long long hi)
{
    const std::string low = std::to_string(lo);
    const std::string high = std::to_string(hi);
    if (low.size() == high.size()) {
        return SameLengthAlternatives(low, high);
    }
    std::vector<std::string> alternatives;
    // The shortest band, from lo up to all nines; it is full when lo is a power of ten.
    std::size_t firstFull = low.size(); // The digit count of the first full band.
    if (low != "1" + std::string(low.size() - 1, '0')) {
        alternatives = SameLengthAlternatives(low, std::string(low.size(), '9'));
        firstFull = low.size() + 1;
    }
    // The longest band, from its power of ten up to hi; it is full when hi is all nines.
    const bool highFull = high == std::string(high.size(), '9');
    const std::size_t lastFull = highFull ? high.size() : high.size() - 1;
    if (firstFull <= lastFull) {
        alternatives.push_back(lastFull == 1 ? std::string("[1-9]") : "[1-9] " + AnyDigits(firstFull - 1, lastFull - 1));
    }
    if (!highFull) {
        const std::vector<std::string> top = SameLengthAlternatives("1" + std::string(high.size() - 1, '0'), high);
        alternatives.insert(alternatives.end(), top.begin(), top.end());
    }
    return alternatives;
}

} // namespace
```

and replace

```cpp
std::string Enum(const std::vector<std::string>& values)
{
```

with

```cpp
std::string IntRange(long long min, long long max)
{
    MORPHEUS_REQUIRE(min <= max, "IntRange: min must not exceed max");
    // Magnitudes are unsigned, so the magnitude of the lowest long long (2^63) fits.
    auto magnitude = [](long long value) { return 0ULL - static_cast<unsigned long long>(value); };
    std::vector<std::string> alternatives;
    // The negative numbers: "-" and a magnitude of 1 or more, so "-0" cannot be written.
    if (min < 0) {
        const unsigned long long fewest = max < 0 ? magnitude(max) : 1ULL;
        alternatives.push_back(Literal("-") + " " + Alternatives(MagnitudeAlternatives(fewest, magnitude(min))));
    }
    // Zero is written "0" and nothing else.
    if (min <= 0 && max >= 0) {
        alternatives.push_back(Literal("0"));
    }
    // The positive numbers.
    if (max > 0) {
        const unsigned long long fewest = min > 0 ? static_cast<unsigned long long>(min) : 1ULL;
        const std::vector<std::string> positive = MagnitudeAlternatives(fewest, static_cast<unsigned long long>(max));
        alternatives.insert(alternatives.end(), positive.begin(), positive.end());
    }
    return Alternatives(alternatives);
}

std::string Enum(const std::vector<std::string>& values)
{
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[int-range]"`
Expected: builds with no warnings; `All tests passed` (3 test cases). If a hand-computed text differs, recheck it against the algorithm above before touching the code: the brute force is the arbiter of exactness.

- [ ] **Step 6: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 335`.

- [ ] **Step 7: Commit**

```bash
git add src/grammar/Gbnf.h src/grammar/Gbnf.cpp tests/grammar/GbnfTests.cpp
git commit -m "Add IntRange: exact decimal integer ranges as GBNF" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

