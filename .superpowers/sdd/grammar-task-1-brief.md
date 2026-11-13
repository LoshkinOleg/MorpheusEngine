### Task 1: A test-only GBNF matcher

The fast suite cannot run llama.cpp, so it gets a small matcher for the GBNF the builder and today's grammars write. It parses one rule per line into a tree and matches by sets of end positions (every place an expression can end when it starts at a position), over the text's code points.

**Files:**
- Create: `tests/grammar/GbnfMatcher.h`
- Test: `tests/grammar/GbnfMatcherTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `class morpheus::test::GbnfMatcher` with `explicit GbnfMatcher(const std::string& grammar)` (throws `std::runtime_error` for a grammar it cannot read, a reference to an undefined rule, no `root`, a rule defined twice) and `bool Accepts(std::string_view text) const` (the root rule matches all of `text`; invalid UTF-8 never matches; a rule that comes back to itself without consuming text throws `std::runtime_error`).

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/GbnfMatcherTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "grammar/GbnfMatcher.h"

using namespace morpheus;

TEST_CASE("The test GBNF matcher reads literals, classes, groups and repetitions", "[gbnf-matcher]")
{
    const test::GbnfMatcher matcher(R"gbnf(root ::= "a" ("b" | "cd")? [x-z]{1,2} digits
digits ::= [0-9]*
)gbnf");
    CHECK(matcher.Accepts("ax"));
    CHECK(matcher.Accepts("abxy"));
    CHECK(matcher.Accepts("acdz123"));
    CHECK_FALSE(matcher.Accepts("a"));     // [x-z] at least once
    CHECK_FALSE(matcher.Accepts("axyz"));  // [x-z] at most twice
    CHECK_FALSE(matcher.Accepts("abcdx")); // "b" or "cd", not both
    CHECK_FALSE(matcher.Accepts("ax1a"));  // the root must match the whole text
    CHECK_FALSE(matcher.Accepts(""));

    const test::GbnfMatcher counts(R"gbnf(root ::= "<" [0-9]{2} ("," [0-9]){0,1} [a]+ [^a-z]{1,} ">"
)gbnf");
    CHECK(counts.Accepts("<12aA>"));
    CHECK(counts.Accepts("<12,3aaa!?>"));
    CHECK_FALSE(counts.Accepts("<1aA>"));      // {2} is exactly two
    CHECK_FALSE(counts.Accepts("<12,3,4aA>")); // {0,1} is at most one
    CHECK_FALSE(counts.Accepts("<12A>"));      // + needs one
    CHECK_FALSE(counts.Accepts("<12aa>"));     // {1,} needs one
}

TEST_CASE("The test GBNF matcher decodes escapes and UTF-8 as llama.cpp does", "[gbnf-matcher]")
{
    // The JSON string character of the grammar builder: a code point but ", \, DEL and the controls, or one escape.
    const test::GbnfMatcher matcher(R"gbnf(root ::= "\"" char{0,3} "\""
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
)gbnf");
    CHECK(matcher.Accepts(R"("")"));
    CHECK(matcher.Accepts(R"("abc")"));
    CHECK(matcher.Accepts("\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"")); // three characters of 2, 3 and 4 bytes
    CHECK(matcher.Accepts(R"("\n\"\u00e9")"));                          // three escapes
    CHECK_FALSE(matcher.Accepts(R"("abcd")"));                         // four characters
    CHECK_FALSE(matcher.Accepts("\"a\nb\""));                           // a raw control character
    CHECK_FALSE(matcher.Accepts("\"\x7F\""));                           // DEL
    CHECK_FALSE(matcher.Accepts(R"("\q")"));                           // not a JSON escape
    CHECK_FALSE(matcher.Accepts("\"\xC3\""));                           // not UTF-8

    // Literal escapes: \x, \u, \n and the quote.
    const test::GbnfMatcher escapes(R"gbnf(root ::= "\x41\u00e9\n\"" [\x30-\x32]
)gbnf");
    CHECK(escapes.Accepts("A\xC3\xA9\n\"1"));
    CHECK_FALSE(escapes.Accepts("A\xC3\xA9\n\"3"));
}

TEST_CASE("The test GBNF matcher refuses grammars it cannot read", "[gbnf-matcher]")
{
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= missing\n"), std::runtime_error);              // an undefined rule
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= (\"a\"\n"), std::runtime_error);               // an unclosed group
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"a\n"), std::runtime_error);                  // an unclosed literal
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= [a-z\n"), std::runtime_error);                 // an unclosed class
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"a\")\n"), std::runtime_error);               // an unmatched ')'
    CHECK_THROWS_AS(test::GbnfMatcher("other ::= \"a\"\n"), std::runtime_error);               // no root
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"a\"\nroot ::= \"b\"\n"), std::runtime_error); // defined twice
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"\\q\"\n"), std::runtime_error);              // an unknown escape
    CHECK_THROWS_AS(test::GbnfMatcher("root \"a\"\n"), std::runtime_error);                     // no ' ::= '

    // A rule that comes back to itself without consuming text would loop: it is refused when matched.
    const test::GbnfMatcher recursive("root ::= a\na ::= root | \"x\"\n");
    CHECK_THROWS_AS(recursive.Accepts("x"), std::runtime_error);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/GbnfMatcher.h': No such file or directory` in `tests/grammar/GbnfMatcherTests.cpp`.

- [ ] **Step 3: Write the matcher**

Create `tests/grammar/GbnfMatcher.h`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morpheus::test {

// A test-only matcher for the GBNF that grammar::GrammarBuilder and Morpheus's grammars write, so the fast suite can
// check what a grammar accepts without llama.cpp (the [llama] tests check that llama.cpp agrees). It reads one rule
// per line, "name ::= body". A body has string literals ("..." with the escapes \" \\ \n \r \t \[ \] \xHH \uHHHH
// \UHHHHHHHH), character classes ([...] and [^...], with ranges and the same escapes), rule references, groups, "|"
// and the postfixes ?, *, +, {m}, {m,} and {m,n}. Like llama.cpp it matches the code points of the UTF-8 text, and the
// root rule must match all of it. Throws std::runtime_error for a grammar it cannot read, a reference to an undefined
// rule, or a rule that comes back to itself without consuming text.
class GbnfMatcher {
private:
    // One parsed expression.
    struct Node {
        enum class Kind { Literal, Class, Reference, Sequence, Choice, Repeat };

        Kind kind = Kind::Sequence;
        std::u32string text = {};                                // Literal: the code points to match.
        std::vector<std::pair<char32_t, char32_t>> ranges = {}; // Class: inclusive code point ranges.
        bool negated = false;                                    // Class: [^...].
        std::string rule = {};                                   // Reference: the rule's name.
        std::vector<Node> children = {};                         // Sequence, Choice: the parts; Repeat: the one item.
        std::size_t fewest = 0;                                  // Repeat: the least repetitions.
        std::size_t most = 0;                                    // Repeat: the most repetitions, or NO_LIMIT.
    };

    static constexpr std::size_t NO_LIMIT = static_cast<std::size_t>(-1);

    std::map<std::string, Node> _rules = {};

public:
    explicit GbnfMatcher(const std::string& grammar)
    {
        // One rule per line; blank lines are skipped.
        std::size_t start = 0;
        while (start < grammar.size()) {
            std::size_t end = grammar.find('\n', start);
            if (end == std::string::npos) {
                end = grammar.size();
            }
            const std::string line = grammar.substr(start, end - start);
            start = end + 1;
            if (line.empty()) {
                continue;
            }
            const std::size_t arrow = line.find(" ::= ");
            if (arrow == std::string::npos) {
                throw std::runtime_error("GbnfMatcher: a line without ' ::= ': " + line);
            }
            const std::string name = line.substr(0, arrow);
            std::size_t pos = arrow + 5;
            Node body = ParseChoice(line, pos);
            // ParseChoice stops at a ")" it did not open, or at the end of the line.
            if (pos != line.size()) {
                throw std::runtime_error("GbnfMatcher: an unmatched ')' in: " + line);
            }
            if (!_rules.emplace(name, std::move(body)).second) {
                throw std::runtime_error("GbnfMatcher: '" + name + "' is defined twice");
            }
        }
        if (!_rules.contains("root")) {
            throw std::runtime_error("GbnfMatcher: no root rule");
        }
        // Every reference must name a rule.
        for (const auto& [name, body] : _rules) {
            CheckReferences(body, name);
        }
    }

    // Whether the root rule matches all of `text`. Text that is not UTF-8 never matches.
    bool Accepts(std::string_view text) const
    {
        std::u32string codePoints;
        for (std::size_t pos = 0; pos < text.size();) {
            char32_t codePoint = 0;
            if (!DecodeUtf8(text, pos, codePoint)) {
                return false;
            }
            codePoints += codePoint;
        }
        std::set<std::pair<std::string, std::size_t>> active;
        return Match(_rules.at("root"), codePoints, 0, active).contains(codePoints.size());
    }

private:
    // Alternatives separated by "|", up to a ")" or the end of the line.
    Node ParseChoice(const std::string& line, std::size_t& pos) const
    {
        Node choice;
        choice.kind = Node::Kind::Choice;
        choice.children.push_back(ParseSequence(line, pos));
        while (pos < line.size() && line[pos] == '|') {
            ++pos;
            choice.children.push_back(ParseSequence(line, pos));
        }
        return choice;
    }

    // Items, each with its postfixes, up to a "|", a ")" or the end of the line.
    Node ParseSequence(const std::string& line, std::size_t& pos) const
    {
        Node sequence;
        sequence.kind = Node::Kind::Sequence;
        for (;;) {
            while (pos < line.size() && line[pos] == ' ') {
                ++pos;
            }
            if (pos >= line.size() || line[pos] == '|' || line[pos] == ')') {
                return sequence;
            }
            Node item = ParsePrimary(line, pos);
            // Each postfix wraps the item in a repetition: ? is {0,1}, * is {0,}, + is {1,}.
            while (pos < line.size() && (line[pos] == '?' || line[pos] == '*' || line[pos] == '+' || line[pos] == '{')) {
                Node repeat;
                repeat.kind = Node::Kind::Repeat;
                const char postfix = line[pos++];
                if (postfix == '?') {
                    repeat.fewest = 0;
                    repeat.most = 1;
                } else if (postfix == '*') {
                    repeat.fewest = 0;
                    repeat.most = NO_LIMIT;
                } else if (postfix == '+') {
                    repeat.fewest = 1;
                    repeat.most = NO_LIMIT;
                } else {
                    // {m}, {m,} or {m,n}.
                    const std::size_t close = line.find('}', pos);
                    if (close == std::string::npos) {
                        throw std::runtime_error("GbnfMatcher: an unclosed '{' in: " + line);
                    }
                    const std::string counts = line.substr(pos, close - pos);
                    pos = close + 1;
                    const std::size_t comma = counts.find(',');
                    repeat.fewest = static_cast<std::size_t>(std::stoull(counts.substr(0, comma)));
                    if (comma == std::string::npos) {
                        repeat.most = repeat.fewest;
                    } else if (comma + 1 == counts.size()) {
                        repeat.most = NO_LIMIT;
                    } else {
                        repeat.most = static_cast<std::size_t>(std::stoull(counts.substr(comma + 1)));
                    }
                }
                repeat.children.push_back(std::move(item));
                item = std::move(repeat);
            }
            sequence.children.push_back(std::move(item));
        }
    }

    // A string literal, a character class, a group or a rule reference.
    Node ParsePrimary(const std::string& line, std::size_t& pos) const
    {
        Node node;
        if (line[pos] == '"') {
            node.kind = Node::Kind::Literal;
            ++pos;
            while (pos < line.size() && line[pos] != '"') {
                node.text += ParseChar(line, pos);
            }
            if (pos >= line.size()) {
                throw std::runtime_error("GbnfMatcher: an unclosed string literal in: " + line);
            }
            ++pos;
        } else if (line[pos] == '[') {
            node.kind = Node::Kind::Class;
            ++pos;
            if (pos < line.size() && line[pos] == '^') {
                node.negated = true;
                ++pos;
            }
            while (pos < line.size() && line[pos] != ']') {
                // One character, or a range "a-b" (a "-" just before "]" is a plain character).
                const char32_t low = ParseChar(line, pos);
                char32_t high = low;
                if (pos + 1 < line.size() && line[pos] == '-' && line[pos + 1] != ']') {
                    ++pos;
                    high = ParseChar(line, pos);
                }
                node.ranges.emplace_back(low, high);
            }
            if (pos >= line.size()) {
                throw std::runtime_error("GbnfMatcher: an unclosed character class in: " + line);
            }
            ++pos;
        } else if (line[pos] == '(') {
            ++pos;
            node = ParseChoice(line, pos);
            if (pos >= line.size() || line[pos] != ')') {
                throw std::runtime_error("GbnfMatcher: an unclosed '(' in: " + line);
            }
            ++pos;
        } else if (IsNameChar(line[pos])) {
            node.kind = Node::Kind::Reference;
            while (pos < line.size() && IsNameChar(line[pos])) {
                node.rule += line[pos++];
            }
        } else {
            throw std::runtime_error(std::string("GbnfMatcher: an unexpected '") + line[pos] + "' in: " + line);
        }
        return node;
    }

    // One character of a literal or a class: an escape, or one UTF-8 code point.
    char32_t ParseChar(const std::string& line, std::size_t& pos) const
    {
        if (line[pos] != '\\') {
            char32_t codePoint = 0;
            if (!DecodeUtf8(line, pos, codePoint)) {
                throw std::runtime_error("GbnfMatcher: invalid UTF-8 in: " + line);
            }
            return codePoint;
        }
        if (pos + 1 >= line.size()) {
            throw std::runtime_error("GbnfMatcher: an escape at the end of: " + line);
        }
        const char escape = line[pos + 1];
        pos += 2;
        // The single-character escapes, or the number of hex digits that follow.
        std::size_t digits = 0;
        switch (escape) {
        case 'n':
            return U'\n';
        case 'r':
            return U'\r';
        case 't':
            return U'\t';
        case '\\':
        case '"':
        case '[':
        case ']':
            return static_cast<char32_t>(escape);
        case 'x':
            digits = 2;
            break;
        case 'u':
            digits = 4;
            break;
        case 'U':
            digits = 8;
            break;
        default:
            throw std::runtime_error(std::string("GbnfMatcher: an unknown escape '\\") + escape + "' in: " + line);
        }
        if (pos + digits > line.size()) {
            throw std::runtime_error("GbnfMatcher: a short hex escape in: " + line);
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            const char c = line[pos + i];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                throw std::runtime_error("GbnfMatcher: a bad hex digit in: " + line);
            }
            value = value * 16 + digit;
        }
        pos += digits;
        return static_cast<char32_t>(value);
    }

    // Throws unless every reference inside `node` (part of the rule `name`) names a rule. Recursive over the tree.
    void CheckReferences(const Node& node, const std::string& name) const
    {
        if (node.kind == Node::Kind::Reference && !_rules.contains(node.rule)) {
            throw std::runtime_error("GbnfMatcher: '" + node.rule + "' used by '" + name + "' is not defined");
        }
        for (const Node& child : node.children) {
            CheckReferences(child, name);
        }
    }

    // The positions where `node` can end when it starts at `pos` of `text`. `active` holds the rule references being
    // matched with their positions, so a rule that comes back to itself at the same position throws instead of
    // looping. Recursive over the tree.
    std::set<std::size_t> Match(const Node& node, const std::u32string& text, std::size_t pos,
                                std::set<std::pair<std::string, std::size_t>>& active) const
    {
        std::set<std::size_t> ends;
        switch (node.kind) {
        case Node::Kind::Literal:
            // compare() takes what is left when the text is shorter, which then differs from the literal.
            if (text.compare(pos, node.text.size(), node.text) == 0) {
                ends.insert(pos + node.text.size());
            }
            break;
        case Node::Kind::Class:
            if (pos < text.size()) {
                const char32_t c = text[pos];
                const bool inRanges = std::any_of(node.ranges.begin(), node.ranges.end(),
                                                  [c](const auto& range) { return range.first <= c && c <= range.second; });
                if (inRanges != node.negated) {
                    ends.insert(pos + 1);
                }
            }
            break;
        case Node::Kind::Reference: {
            const std::pair<std::string, std::size_t> call(node.rule, pos);
            if (!active.insert(call).second) {
                throw std::runtime_error("GbnfMatcher: '" + node.rule + "' comes back to itself without consuming text");
            }
            ends = Match(_rules.at(node.rule), text, pos, active);
            active.erase(call);
            break;
        }
        case Node::Kind::Sequence: {
            // Each part starts wherever the parts before it can end.
            ends.insert(pos);
            for (const Node& child : node.children) {
                std::set<std::size_t> next;
                for (const std::size_t start : ends) {
                    const std::set<std::size_t> more = Match(child, text, start, active);
                    next.insert(more.begin(), more.end());
                }
                ends = std::move(next);
                if (ends.empty()) {
                    break;
                }
            }
            break;
        }
        case Node::Kind::Choice:
            for (const Node& child : node.children) {
                const std::set<std::size_t> more = Match(child, text, pos, active);
                ends.insert(more.begin(), more.end());
            }
            break;
        case Node::Kind::Repeat: {
            // Without a limit, more repetitions than characters left beyond `fewest` cannot reach a new position.
            const std::size_t most = node.most == NO_LIMIT ? node.fewest + (text.size() - pos) : node.most;
            std::set<std::size_t> current = {pos};
            if (node.fewest == 0) {
                ends.insert(pos);
            }
            for (std::size_t count = 1; count <= most && !current.empty(); ++count) {
                std::set<std::size_t> next;
                for (const std::size_t start : current) {
                    const std::set<std::size_t> more = Match(node.children[0], text, start, active);
                    next.insert(more.begin(), more.end());
                }
                current = std::move(next);
                if (count >= node.fewest) {
                    ends.insert(current.begin(), current.end());
                }
            }
            break;
        }
        }
        return ends;
    }

    static bool IsNameChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    }

    // Decodes the UTF-8 code point at `pos` and moves past it; false for bytes that are not UTF-8.
    static bool DecodeUtf8(std::string_view text, std::size_t& pos, char32_t& codePoint)
    {
        auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
        const unsigned char lead = byte(pos);
        std::size_t length = 0;
        std::uint32_t value = 0;
        if (lead < 0x80) {
            length = 1;
            value = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
            value = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            value = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            value = lead & 0x07u;
        } else {
            return false;
        }
        if (pos + length > text.size()) {
            return false;
        }
        // Each continuation byte is 10xxxxxx and adds six bits.
        for (std::size_t i = 1; i < length; ++i) {
            if ((byte(pos + i) & 0xC0) != 0x80) {
                return false;
            }
            value = (value << 6) | (byte(pos + i) & 0x3Fu);
        }
        pos += length;
        codePoint = static_cast<char32_t>(value);
        return true;
    }
};

} // namespace morpheus::test
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[gbnf-matcher]"`
Expected: builds with no warnings; `All tests passed` (3 test cases).

- [ ] **Step 5: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 318`.

- [ ] **Step 6: Commit**

```bash
git add tests/grammar/GbnfMatcher.h tests/grammar/GbnfMatcherTests.cpp
git commit -m "Add a test-only GBNF matcher for the fast suite" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

