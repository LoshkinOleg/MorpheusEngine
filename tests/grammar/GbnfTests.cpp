#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "grammar/Gbnf.h"
#include "grammar/GbnfMatcher.h"

using namespace morpheus;

namespace {

// The shared `char` terminal, as GrammarBuilder::Text appends it.
const std::string CHAR_LINE =
    R"gbnf(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))gbnf" "\n";

// A matcher for the grammar whose root is `rootBody`.
test::GbnfMatcher MatcherFor(const std::string& rootBody)
{
    return test::GbnfMatcher(grammar::GrammarBuilder().Text(rootBody));
}

} // namespace

TEST_CASE("Literal matches its text exactly, with quotes and backslashes escaped", "[gbnf]")
{
    CHECK(grammar::Literal("abc") == R"("abc")");
    CHECK(grammar::Literal(R"(a"b\c)") == R"("a\"b\\c")");
    CHECK(grammar::Literal("") == R"("")");
    CHECK(grammar::Literal("caf\xC3\xA9") == "\"caf\xC3\xA9\"");
    // A grammar is one rule per line, so a literal cannot hold a line break.
    CHECK_THROWS_AS(grammar::Literal("a\nb"), core::ContractError);
    CHECK_THROWS_AS(grammar::Literal("a\rb"), core::ContractError);
}

TEST_CASE("JsonString bounds a JSON string by characters", "[gbnf]")
{
    CHECK(grammar::JsonString(0, 120) == R"gbnf("\"" char{0,120} "\"")gbnf");
    CHECK(grammar::JsonString(1, 200) == R"gbnf("\"" char{1,200} "\"")gbnf");
    CHECK(grammar::JsonString(3, 3) == R"gbnf("\"" char{3} "\"")gbnf");
    CHECK(grammar::JsonString(0, 0) == R"gbnf("\"\"")gbnf");
    CHECK_THROWS_AS(grammar::JsonString(-1, 2), core::ContractError);
    CHECK_THROWS_AS(grammar::JsonString(3, 2), core::ContractError);

    // Each character is a code point or one escape; raw control characters, DEL and lone quotes are refused.
    const test::GbnfMatcher text = MatcherFor(grammar::JsonString(1, 3));
    CHECK(text.Accepts(R"("a")"));
    CHECK(text.Accepts(R"("\u00e9\n\"")"));
    CHECK(text.Accepts("\"\xE6\x97\xA5\xF0\x9F\x98\x80\xC3\xA9\""));
    CHECK_FALSE(text.Accepts(R"("")"));
    CHECK_FALSE(text.Accepts(R"("abcd")"));
    CHECK_FALSE(text.Accepts("\"a\tb\""));
    CHECK_FALSE(text.Accepts("\"\x7F\""));
    CHECK_FALSE(text.Accepts(R"("a"")"));
    CHECK_FALSE(text.Accepts("a"));
}

TEST_CASE("JsonString stays within llama.cpp's repetition limit", "[gbnf]")
{
    // `char` repeats up to maxChars times, and llama.cpp refuses a count above GBNF_MAX_REPETITION.
    CHECK(grammar::JsonString(0, grammar::GBNF_MAX_REPETITION) == R"gbnf("\"" char{0,2000} "\"")gbnf");
    CHECK_THROWS_AS(grammar::JsonString(0, grammar::GBNF_MAX_REPETITION + 1), core::ContractError);
}

TEST_CASE("Enum matches one of its values as a JSON string", "[gbnf]")
{
    CHECK(grammar::Enum({"wary"}) == R"gbnf("\"wary\"")gbnf");
    CHECK(grammar::Enum({"wary", "friendly"}) == R"gbnf(("\"wary\"" | "\"friendly\""))gbnf");
    // The value is JSON-escaped, then escaped once more as a GBNF literal.
    CHECK(grammar::Enum({R"(a"b\c)"}) == R"gbnf("\"a\\\"b\\\\c\"")gbnf");
    CHECK_THROWS_AS(grammar::Enum({}), core::ContractError);
    CHECK_THROWS_AS(grammar::Enum({"a", "b", "a"}), core::ContractError);

    const test::GbnfMatcher mood = MatcherFor(grammar::Enum({"wary", R"(a"b)"}));
    CHECK(mood.Accepts(R"("wary")"));
    CHECK(mood.Accepts(R"("a\"b")"));
    CHECK_FALSE(mood.Accepts("wary"));
    CHECK_FALSE(mood.Accepts(R"("friendly")"));
}

TEST_CASE("ListOf bounds a compact JSON array", "[gbnf]")
{
    CHECK(grammar::ListOf("x", 0, 3) == R"gbnf(("[]" | "[" x ("," x){0,2} "]"))gbnf");
    CHECK(grammar::ListOf("x", 1, 3) == R"gbnf("[" x ("," x){0,2} "]")gbnf");
    CHECK(grammar::ListOf("x", 2, 5) == R"gbnf("[" x ("," x){1,4} "]")gbnf");
    CHECK(grammar::ListOf("x", 2, 2) == R"gbnf("[" x ("," x){1} "]")gbnf");
    CHECK(grammar::ListOf("x", 0, 1) == R"gbnf(("[]" | "[" x "]"))gbnf");
    CHECK(grammar::ListOf("x", 1, 1) == R"gbnf("[" x "]")gbnf");
    CHECK_THROWS_AS(grammar::ListOf("x", 0, 0), core::ContractError);
    CHECK_THROWS_AS(grammar::ListOf("x", -1, 2), core::ContractError);
    CHECK_THROWS_AS(grammar::ListOf("x", 3, 2), core::ContractError);

    const test::GbnfMatcher list = MatcherFor(grammar::ListOf(grammar::Literal("7"), 0, 2));
    CHECK(list.Accepts("[]"));
    CHECK(list.Accepts("[7]"));
    CHECK(list.Accepts("[7,7]"));
    CHECK_FALSE(list.Accepts("[7,7,7]"));
    CHECK_FALSE(list.Accepts("[ 7]"));
    CHECK_FALSE(list.Accepts("[7,]"));
    CHECK_FALSE(list.Accepts("[,]"));
}

TEST_CASE("ListOf stays within llama.cpp's repetition limit", "[gbnf]")
{
    // The items after the first repeat up to maxItems - 1 times, and llama.cpp refuses a count above
    // GBNF_MAX_REPETITION.
    CHECK(grammar::ListOf("x", 1, grammar::GBNF_MAX_REPETITION + 1) == R"gbnf("[" x ("," x){0,2000} "]")gbnf");
    CHECK_THROWS_AS(grammar::ListOf("x", 1, grammar::GBNF_MAX_REPETITION + 2), core::ContractError);
}

TEST_CASE("Object matches a compact JSON object with fixed keys in a fixed order", "[gbnf]")
{
    CHECK(grammar::Object({{"a", "x"}}) == R"gbnf("{\"a\":" x "}")gbnf");
    CHECK(grammar::Object({{"a", "x"}, {"b", "y"}}) == R"gbnf("{\"a\":" x ",\"b\":" y "}")gbnf");
    CHECK(grammar::Object({{"say \"hi\"", "x"}}) == R"gbnf("{\"say \\\"hi\\\"\":" x "}")gbnf");
    CHECK_THROWS_AS(grammar::Object({}), core::ContractError);
    CHECK_THROWS_AS(grammar::Object({{"a", "x"}, {"a", "y"}}), core::ContractError);

    const test::GbnfMatcher object =
        MatcherFor(grammar::Object({{"a", grammar::Enum({"x", "y"})}, {"b", grammar::Literal("1")}}));
    CHECK(object.Accepts(R"({"a":"x","b":1})"));
    CHECK(object.Accepts(R"({"a":"y","b":1})"));
    CHECK_FALSE(object.Accepts(R"({"b":1,"a":"x"})"));
    CHECK_FALSE(object.Accepts(R"({"a":"x"})"));
    CHECK_FALSE(object.Accepts(R"({"a":"x","b":1,"c":1})"));
    CHECK_FALSE(object.Accepts(R"({"a": "x","b":1})"));
    CHECK_FALSE(object.Accepts(R"({"a":"z","b":1})"));
}

TEST_CASE("Alternatives matches any one of its expressions and can stand in a sequence", "[gbnf]")
{
    CHECK(grammar::Alternatives({"a"}) == "a");
    CHECK(grammar::Alternatives({"a", "b c"}) == "(a | b c)");
    CHECK_THROWS_AS(grammar::Alternatives({}), core::ContractError);

    const test::GbnfMatcher either = MatcherFor(grammar::Literal("<") + " " +
                                                grammar::Alternatives({grammar::Literal("yes"), grammar::Literal("no")}) +
                                                " " + grammar::Literal(">"));
    CHECK(either.Accepts("<yes>"));
    CHECK(either.Accepts("<no>"));
    CHECK_FALSE(either.Accepts("<yes"));
    CHECK_FALSE(either.Accepts("no>"));
    CHECK_FALSE(either.Accepts("<yesno>"));
}

TEST_CASE("GrammarBuilder writes the root line first, then its rules, then the shared terminals used", "[gbnf]")
{
    grammar::GrammarBuilder builder;
    CHECK(builder.Rule("item-list", grammar::ListOf("item", 0, 2)) == "item-list");
    CHECK(builder.Rule("item", grammar::JsonString(1, 4)) == "item");
    CHECK(builder.Text(grammar::Object({{"items", "item-list"}})) == R"gbnf(root ::= "{\"items\":" item-list "}"
item-list ::= ("[]" | "[" item ("," item){0,1} "]")
item ::= "\"" char{1,4} "\""
)gbnf" + CHAR_LINE);

    // Without a JSON string there is no `char` rule.
    CHECK(grammar::GrammarBuilder().Text(grammar::Literal("yes")) == "root ::= \"yes\"\n");
}

TEST_CASE("GrammarBuilder refuses broken rules and undefined references", "[gbnf]")
{
    grammar::GrammarBuilder builder;
    builder.Rule("a", grammar::Literal("a"));
    CHECK_THROWS_AS(builder.Rule("a", grammar::Literal("b")), core::ContractError);          // defined twice
    CHECK_THROWS_AS(builder.Rule("Upper", grammar::Literal("b")), core::ContractError);      // not [a-z0-9-]
    CHECK_THROWS_AS(builder.Rule("snake_case", grammar::Literal("b")), core::ContractError); // not [a-z0-9-]
    CHECK_THROWS_AS(builder.Rule("two words", grammar::Literal("b")), core::ContractError);  // not [a-z0-9-]
    CHECK_THROWS_AS(builder.Rule("", grammar::Literal("b")), core::ContractError);
    CHECK_THROWS_AS(builder.Rule("root", grammar::Literal("b")), core::ContractError);       // reserved
    CHECK_THROWS_AS(builder.Rule("char", grammar::Literal("b")), core::ContractError);       // reserved
    CHECK_THROWS_AS(builder.Rule("b", ""), core::ContractError);                             // an empty body
    CHECK_THROWS_AS(builder.Rule("b", "a\na"), core::ContractError);                         // two lines
    CHECK_THROWS_AS(builder.Text(""), core::ContractError);
    CHECK_THROWS_AS(builder.Text("a\na"), core::ContractError);

    // Every name the root and the rules use must be defined.
    CHECK(builder.Text("a") == "root ::= a\na ::= \"a\"\n");
    CHECK_THROWS_AS(builder.Text("a missing"), core::ContractError);
    builder.Rule("b", "a undefined-rule");
    CHECK_THROWS_AS(builder.Text("a"), core::ContractError);
}

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
