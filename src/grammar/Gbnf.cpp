#include "grammar/Gbnf.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <set>

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
