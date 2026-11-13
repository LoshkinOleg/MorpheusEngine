### Task 3: The `morpheus_grammar` library and the builder

The library and every builder function except `IntRange` (Task 4). Each function returns an expression that can stand in a GBNF sequence; `GrammarBuilder` writes the root line first.

**Files:**
- Create: `src/grammar/CMakeLists.txt`, `src/grammar/Gbnf.h`, `src/grammar/Gbnf.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/grammar/GbnfTests.cpp`

**Interfaces:**
- Consumes: `MORPHEUS_REQUIRE` and `core::ContractError` (`core/Errors.h`); `test::GbnfMatcher` (Task 1).
- Produces (namespace `morpheus::grammar`, header `grammar/Gbnf.h`):
  - `struct ObjectField { std::string key = {}; std::string value = {}; };`
  - `std::string Literal(std::string_view text);`
  - `std::string JsonString(int minChars, int maxChars);`
  - `std::string Enum(const std::vector<std::string>& values);`
  - `std::string ListOf(std::string_view element, int minItems, int maxItems);`
  - `std::string Object(const std::vector<ObjectField>& fields);`
  - `std::string Alternatives(const std::vector<std::string>& expressions);`
  - `class GrammarBuilder` with `std::string Rule(std::string_view name, std::string body);` and `std::string Text(std::string_view rootBody) const;`
  - CMake target `morpheus_grammar`, alias `Morpheus::grammar`; `morpheus_tests` links it.

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/GbnfTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/Gbnf.h': No such file or directory` in `tests/grammar/GbnfTests.cpp`.

- [ ] **Step 3: Define the library in CMake**

Create `src/grammar/CMakeLists.txt`:

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
)
add_library(Morpheus::grammar ALIAS morpheus_grammar) # The name host projects link.
target_include_directories(morpheus_grammar PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_grammar PUBLIC morpheus_core nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_grammar)
```

In `CMakeLists.txt`, replace

```cmake
add_subdirectory(src/context)
add_subdirectory(src/turn)
```

with

```cmake
add_subdirectory(src/context)
add_subdirectory(src/grammar)
add_subdirectory(src/turn)
```

In `tests/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_tests PRIVATE morpheus_core morpheus_store morpheus_inference morpheus_context morpheus_turn morpheus_app_model morpheus_app_ui imgui_core Catch2::Catch2WithMain)
```

with

```cmake
target_link_libraries(morpheus_tests PRIVATE morpheus_core morpheus_store morpheus_inference morpheus_context morpheus_grammar morpheus_turn morpheus_app_model morpheus_app_ui imgui_core Catch2::Catch2WithMain)
```

- [ ] **Step 4: Write the header**

Create `src/grammar/Gbnf.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morpheus::grammar {

// GBNF text fragments for llama.cpp grammars of compact JSON: no optional whitespace, so the model's answer is one
// line. Every function returns an expression that can stand anywhere in a GBNF sequence: one made of alternatives
// comes in parentheses. A broken contract (for example min > max) is a core::ContractError.

// One key of an Object and the expression for its value.
struct ObjectField {
    std::string key = {};   // The key; Object writes it as a JSON string.
    std::string value = {}; // A GBNF expression for the value.
};

// `text` exactly: a GBNF string literal, with `"` and `\` escaped. A grammar is one rule per line, so `text` must not
// hold a line break. Values that sit inside JSON strings are JSON-escaped by the caller first.
std::string Literal(std::string_view text);

// A JSON string of minChars to maxChars characters. Each character is a code point other than `"`, `\`, U+007F and
// the control characters, or one JSON escape: the shared terminal `char`. Requires 0 <= minChars <= maxChars.
std::string JsonString(int minChars, int maxChars);

// One of `values`, each written as a JSON string. Requires at least one value and no duplicates.
std::string Enum(const std::vector<std::string>& values);

// A JSON array of minItems to maxItems `element`s, compact. Requires 0 <= minItems <= maxItems and maxItems >= 1.
std::string ListOf(std::string_view element, int minItems, int maxItems);

// A JSON object with exactly the keys of `fields`, in their order, compact. Requires at least one field and no
// duplicate keys.
std::string Object(const std::vector<ObjectField>& fields);

// Any one of `expressions`. Requires at least one.
std::string Alternatives(const std::vector<std::string>& expressions);

// Collects named rules into a whole grammar.
class GrammarBuilder {
private:
    std::vector<std::pair<std::string, std::string>> _rules = {}; // Name and body, in the order they were added.

public:
    // Adds the rule `name ::= body` and returns `name`, for use in other expressions. Names follow GBNF: [a-z0-9-]+.
    // "root" and the shared terminal's name ("char") are reserved. A duplicate name, or a body that is empty or holds
    // a line break, is a ContractError.
    std::string Rule(std::string_view name, std::string body);

    // The full grammar: the line `root ::= rootBody` first, then the rules in the order they were added, then the
    // shared terminals the root and the rules use (`char`). The root line comes first because turn::CallKindOf
    // recognises a review and a lookup call by their grammar's first line. Every rule name the root and the rules use
    // must be one added here or a shared terminal; an undefined one, or a root body that is empty or holds a line
    // break, is a ContractError.
    std::string Text(std::string_view rootBody) const;
};

} // namespace morpheus::grammar
```

- [ ] **Step 5: Write the implementation**

Create `src/grammar/Gbnf.cpp`:

```cpp
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
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[gbnf]"`
Expected: builds with no warnings; `All tests passed` (8 test cases).

- [ ] **Step 7: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 332`.

- [ ] **Step 8: Commit**

```bash
git add src/grammar/CMakeLists.txt src/grammar/Gbnf.h src/grammar/Gbnf.cpp CMakeLists.txt tests/CMakeLists.txt tests/grammar/GbnfTests.cpp
git commit -m "Add morpheus_grammar with the GBNF builder" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

