#include "grammar/Gbnf.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "core/Errors.h"

namespace morpheus::grammar {

namespace {

// The shared terminal `char`: one character of a JSON string, a code point other than `"`, `\`, DEL and the control
// characters, or one escape. Text appends it when the root or a rule uses it.
constexpr std::string_view CHAR_RULE_NAME = "char";
constexpr std::string_view CHAR_RULE =
    R"gbnf(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))gbnf";

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

std::string Literal(std::string_view text)
{
    MORPHEUS_REQUIRE(text.find_first_of("\r\n") == std::string_view::npos,
                     "Literal: the text must not hold a line break");
    // Inside a GBNF string literal only the quote and the backslash need a backslash.
    std::string result = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result + "\"";
}

std::string JsonString(int minChars, int maxChars)
{
    MORPHEUS_REQUIRE(0 <= minChars && minChars <= maxChars, "JsonString: needs 0 <= minChars <= maxChars");
    // No characters at all: just the two quotes.
    if (maxChars == 0) {
        return Literal("\"\"");
    }
    return Literal("\"") + " char" + Count(static_cast<std::size_t>(minChars), static_cast<std::size_t>(maxChars)) +
           " " + Literal("\"");
}

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
    MORPHEUS_REQUIRE(!values.empty(), "Enum: needs at least one value");
    MORPHEUS_REQUIRE(std::set<std::string>(values.begin(), values.end()).size() == values.size(),
                     "Enum: the values must be distinct");
    // Each value is a JSON string inside the answer, so its JSON text is what the literal matches.
    std::vector<std::string> literals;
    for (const std::string& value : values) {
        literals.push_back(Literal(nlohmann::json(value).dump()));
    }
    return Alternatives(literals);
}

std::string ListOf(std::string_view element, int minItems, int maxItems)
{
    MORPHEUS_REQUIRE(0 <= minItems && minItems <= maxItems && maxItems >= 1,
                     "ListOf: needs 0 <= minItems <= maxItems and maxItems >= 1");
    // The first element is written out; a repetition of "," and an element adds the others.
    const std::size_t fewestMore = static_cast<std::size_t>(std::max(minItems, 1) - 1);
    const std::size_t mostMore = static_cast<std::size_t>(maxItems - 1);
    std::string items = Literal("[") + " " + std::string(element);
    if (mostMore > 0) {
        items += " (" + Literal(",") + " " + std::string(element) + ")" + Count(fewestMore, mostMore);
    }
    items += " " + Literal("]");
    // An empty list is an alternative of its own.
    return minItems == 0 ? Alternatives({Literal("[]"), items}) : items;
}

std::string Object(const std::vector<ObjectField>& fields)
{
    MORPHEUS_REQUIRE(!fields.empty(), "Object: needs at least one field");
    std::set<std::string> keys;
    std::string result;
    for (const ObjectField& field : fields) {
        MORPHEUS_REQUIRE(keys.insert(field.key).second, "Object: the key '" + field.key + "' is given twice");
        // The opening brace or the comma before it, the key as a JSON string and its colon: one literal.
        result += Literal((result.empty() ? "{" : ",") + nlohmann::json(field.key).dump() + ":") + " " + field.value + " ";
    }
    return result + Literal("}");
}

std::string Alternatives(const std::vector<std::string>& expressions)
{
    MORPHEUS_REQUIRE(!expressions.empty(), "Alternatives: needs at least one expression");
    // One expression stands alone; several are grouped, so that the result can stand in a sequence.
    if (expressions.size() == 1) {
        return expressions[0];
    }
    std::string result = "(";
    for (std::size_t i = 0; i < expressions.size(); ++i) {
        result += (i == 0 ? "" : " | ") + expressions[i];
    }
    return result + ")";
}

std::string GrammarBuilder::Rule(std::string_view name, std::string body)
{
    const bool wellFormed = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
    MORPHEUS_REQUIRE(wellFormed, "GrammarBuilder::Rule: '" + std::string(name) + "' is not a GBNF rule name ([a-z0-9-]+)");
    MORPHEUS_REQUIRE(name != "root" && name != CHAR_RULE_NAME,
                     "GrammarBuilder::Rule: '" + std::string(name) + "' is reserved");
    const bool taken = std::any_of(_rules.begin(), _rules.end(), [&](const auto& rule) { return rule.first == name; });
    MORPHEUS_REQUIRE(!taken, "GrammarBuilder::Rule: '" + std::string(name) + "' is defined twice");
    MORPHEUS_REQUIRE(!body.empty() && body.find_first_of("\r\n") == std::string::npos,
                     "GrammarBuilder::Rule: the body of '" + std::string(name) + "' must be one non-empty line");
    _rules.emplace_back(std::string(name), std::move(body));
    return std::string(name);
}

std::string GrammarBuilder::Text(std::string_view rootBody) const
{
    MORPHEUS_REQUIRE(!rootBody.empty() && rootBody.find_first_of("\r\n") == std::string_view::npos,
                     "GrammarBuilder::Text: the root body must be one non-empty line");
    // Every line of the grammar, the root first.
    std::vector<std::pair<std::string, std::string>> lines = {{"root", std::string(rootBody)}};
    lines.insert(lines.end(), _rules.begin(), _rules.end());

    // The rule names the bodies use: the words outside string literals, character classes and repetition counts.
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    std::set<std::string> used;
    for (const auto& [name, body] : lines) {
        std::size_t i = 0;
        while (i < body.size()) {
            const char c = body[i];
            if (c == '"' || c == '[') {
                // A literal or a class: skip to its closer, past backslash escapes.
                const char closer = c == '"' ? '"' : ']';
                std::size_t j = i + 1;
                while (j < body.size() && body[j] != closer) {
                    j += body[j] == '\\' ? 2 : 1;
                }
                MORPHEUS_REQUIRE(j < body.size(), "GrammarBuilder::Text: an unclosed literal or class in '" + name + "'");
                i = j + 1;
            } else if (c == '{') {
                // A repetition count such as {0,5}.
                const std::size_t close = body.find('}', i);
                MORPHEUS_REQUIRE(close != std::string::npos,
                                 "GrammarBuilder::Text: an unclosed repetition in '" + name + "'");
                i = close + 1;
            } else if (isNameChar(c)) {
                std::size_t end = i;
                while (end < body.size() && isNameChar(body[end])) {
                    ++end;
                }
                used.insert(body.substr(i, end - i));
                i = end;
            } else {
                ++i;
            }
        }
    }

    // Each name used must be a rule added here or a shared terminal.
    for (const std::string& reference : used) {
        const bool defined = reference == CHAR_RULE_NAME ||
                             std::any_of(_rules.begin(), _rules.end(),
                                         [&](const auto& rule) { return rule.first == reference; });
        MORPHEUS_REQUIRE(defined, "GrammarBuilder::Text: '" + reference + "' is used but not defined");
    }

    // The root line, the rules in order, then the shared terminals used.
    std::string text;
    for (const auto& [name, body] : lines) {
        text += name + " ::= " + body + "\n";
    }
    if (used.contains(std::string(CHAR_RULE_NAME))) {
        text += std::string(CHAR_RULE) + "\n";
    }
    return text;
}

} // namespace morpheus::grammar
