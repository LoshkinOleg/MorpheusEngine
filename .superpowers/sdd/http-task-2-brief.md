### Task 2: grammar::Check, the test matcher promoted into morpheus_grammar

The matcher moves into the library and is hardened: matching is memoised by (node, position), repetition counts are read strictly, every read error is a `ContractError`, and instead of a yes/no it returns llama.cpp's three reason texts, pointing at the furthest position any partial match reached. Every test that used `test::GbnfMatcher` switches to it, and the header is deleted.

**Files:**
- Create: `src/grammar/Check.h`, `src/grammar/Check.cpp`
- Modify: `src/grammar/CMakeLists.txt`
- Create: `tests/grammar/CheckTests.cpp`
- Delete: `tests/grammar/GbnfMatcher.h`, `tests/grammar/GbnfMatcherTests.cpp`
- Modify: `tests/grammar/GbnfTests.cpp`, `tests/grammar/SchemaTests.cpp`, `tests/grammar/PropertyTests.cpp`, `tests/turn/ResolveGrammarTests.cpp`, `tests/llama/LlamaGrammarTests.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `std::string morpheus::grammar::Check(const std::string& grammar, std::string_view text)` in `grammar/Check.h`: `""` when the root rule matches all of `text`; otherwise `"the grammar rejects '<c>' at the start"`, `"the grammar rejects '<c>' after '<text>'"` or `"the text ends before the grammar does"`. Throws `core::ContractError` for a grammar it cannot read (no root, an undefined rule, a rule defined twice, a malformed literal, class, group, escape or repetition count, a rule that comes back to itself without consuming text).

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/CheckTests.cpp` (the first three test cases are the matcher's own tests, ported):

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "core/Errors.h"
#include "grammar/Check.h"

using namespace morpheus;

TEST_CASE("grammar::Check reads literals, classes, groups and repetitions", "[grammar-check]")
{
    const std::string gbnf = R"gbnf(root ::= "a" ("b" | "cd")? [x-z]{1,2} digits
digits ::= [0-9]*
)gbnf";
    CHECK(grammar::Check(gbnf, "ax").empty());
    CHECK(grammar::Check(gbnf, "abxy").empty());
    CHECK(grammar::Check(gbnf, "acdz123").empty());
    CHECK_FALSE(grammar::Check(gbnf, "a").empty());     // [x-z] at least once
    CHECK_FALSE(grammar::Check(gbnf, "axyz").empty());  // [x-z] at most twice
    CHECK_FALSE(grammar::Check(gbnf, "abcdx").empty()); // "b" or "cd", not both
    CHECK_FALSE(grammar::Check(gbnf, "ax1a").empty());  // the root must match the whole text
    CHECK_FALSE(grammar::Check(gbnf, "").empty());

    const std::string counts = R"gbnf(root ::= "<" [0-9]{2} ("," [0-9]){0,1} [a]+ [^a-z]{1,} ">"
)gbnf";
    CHECK(grammar::Check(counts, "<12aA>").empty());
    CHECK(grammar::Check(counts, "<12,3aaa!?>").empty());
    CHECK_FALSE(grammar::Check(counts, "<1aA>").empty());      // {2} is exactly two
    CHECK_FALSE(grammar::Check(counts, "<12,3,4aA>").empty()); // {0,1} is at most one
    CHECK_FALSE(grammar::Check(counts, "<12A>").empty());      // + needs one
    CHECK_FALSE(grammar::Check(counts, "<12aa>").empty());     // {1,} needs one
}

TEST_CASE("grammar::Check decodes escapes and UTF-8 as llama.cpp does", "[grammar-check]")
{
    // The JSON string character of the grammar builder: a code point but ", \, DEL and the controls, or one escape.
    const std::string jsonString = R"gbnf(root ::= "\"" char{0,3} "\""
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
)gbnf";
    CHECK(grammar::Check(jsonString, R"("")").empty());
    CHECK(grammar::Check(jsonString, R"("abc")").empty());
    CHECK(grammar::Check(jsonString, "\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"").empty()); // three characters of 2, 3 and 4 bytes
    CHECK(grammar::Check(jsonString, R"("\n\"\u00e9")").empty());                          // three escapes
    CHECK_FALSE(grammar::Check(jsonString, R"("abcd")").empty());                         // four characters
    CHECK_FALSE(grammar::Check(jsonString, "\"a\nb\"").empty());                           // a raw control character
    CHECK_FALSE(grammar::Check(jsonString, "\"\x7F\"").empty());                           // DEL
    CHECK_FALSE(grammar::Check(jsonString, R"("\q")").empty());                           // not a JSON escape
    CHECK_FALSE(grammar::Check(jsonString, "\"\xC3\"").empty());                           // not UTF-8

    // Literal escapes: \x, \u, \n and the quote.
    const std::string escapes = R"gbnf(root ::= "\x41\u00e9\n\"" [\x30-\x32]
)gbnf";
    CHECK(grammar::Check(escapes, "A\xC3\xA9\n\"1").empty());
    CHECK_FALSE(grammar::Check(escapes, "A\xC3\xA9\n\"3").empty());
}

TEST_CASE("grammar::Check refuses grammars it cannot read with a ContractError", "[grammar-check]")
{
    CHECK_THROWS_AS(grammar::Check("root ::= missing\n", "x"), core::ContractError);              // an undefined rule
    CHECK_THROWS_AS(grammar::Check("root ::= (\"a\"\n", "x"), core::ContractError);               // an unclosed group
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\n", "x"), core::ContractError);                  // an unclosed literal
    CHECK_THROWS_AS(grammar::Check("root ::= [a-z\n", "x"), core::ContractError);                 // an unclosed class
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\")\n", "x"), core::ContractError);               // an unmatched ')'
    CHECK_THROWS_AS(grammar::Check("other ::= \"a\"\n", "x"), core::ContractError);               // no root
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\"\nroot ::= \"b\"\n", "x"), core::ContractError); // defined twice
    CHECK_THROWS_AS(grammar::Check("root ::= \"\\q\"\n", "x"), core::ContractError);              // an unknown escape
    CHECK_THROWS_AS(grammar::Check("root \"a\"\n", "x"), core::ContractError);                     // no ' ::= '
    CHECK_THROWS_AS(grammar::Check("", "x"), core::ContractError);                                 // nothing to check against

    // A rule that comes back to itself without consuming text would loop: it is refused when matched.
    CHECK_THROWS_AS(grammar::Check("root ::= a\na ::= root | \"x\"\n", "x"), core::ContractError);
}

TEST_CASE("grammar::Check names the character where the text stops fitting", "[grammar-check]")
{
    const std::string yesNo = "root ::= \"{\\\"answer\\\":\\\"\" (\"yes\" | \"no\") \"\\\"}\"\n";
    CHECK(grammar::Check(yesNo, R"({"answer":"yes"})") == "");
    CHECK(grammar::Check(yesNo, R"({"answer":"no"})") == "");

    // The furthest position any partial match reached: the character there is the one rejected.
    CHECK(grammar::Check(yesNo, "x") == "the grammar rejects 'x' at the start");
    CHECK(grammar::Check(yesNo, R"({"answer":"maybe"})") == R"(the grammar rejects 'm' after '{"answer":"')");
    CHECK(grammar::Check(yesNo, R"({"answer":"nope"})") == R"(the grammar rejects 'p' after '{"answer":"no')");
    CHECK(grammar::Check(yesNo, R"({"answer":"yes"} and more)") == R"(the grammar rejects ' ' after '{"answer":"yes"}')");

    // Every path consumed the whole text and wanted more.
    CHECK(grammar::Check(yesNo, R"({"answer":"ye)") == "the text ends before the grammar does");
    CHECK(grammar::Check(yesNo, "") == "the text ends before the grammar does");

    // The rejected character is a whole code point, however many bytes it has.
    CHECK(grammar::Check("root ::= \"caf\" \"e\"\n", "caf\xC3\xA9") == "the grammar rejects '\xC3\xA9' after 'caf'");
}

TEST_CASE("grammar::Check quotes at most the last 60 bytes before the rejected character", "[grammar-check]")
{
    const std::string letters = "root ::= [a-z]* \"!\"\n";
    // Exactly 60 bytes: quoted whole.
    CHECK(grammar::Check(letters, std::string(60, 'a') + "?") ==
          "the grammar rejects '?' after '" + std::string(60, 'a') + "'");
    // More: the last 60, with "..." in front.
    CHECK(grammar::Check(letters, std::string(65, 'a') + "?") ==
          "the grammar rejects '?' after '..." + std::string(60, 'a') + "'");
    // A cut that would fall inside a character starts at the next whole one: 61 bytes before the '?', the first
    // two of them one character, so the cut lands on its second byte and moves past it.
    const std::string before = "\xC3\xA9" + std::string(59, 'a');
    CHECK(grammar::Check("root ::= [^?]* \"!\"\n", before + "?") ==
          "the grammar rejects '?' after '..." + std::string(59, 'a') + "'");
}

TEST_CASE("grammar::Check rejects text that is not UTF-8 at its first bad byte", "[grammar-check]")
{
    // Any code point but NUL: only a byte that is not UTF-8 stops it.
    const std::string anything = "root ::= [^\\x00]*\n";
    CHECK(grammar::Check(anything, "a\xC3\xA9z").empty());
    CHECK(grammar::Check(anything, "a\xFF" "b") == "the grammar rejects '\xFF' after 'a'");
    CHECK(grammar::Check(anything, "ab\xC3") == "the grammar rejects '\xC3' after 'ab'");
}

TEST_CASE("grammar::Check reads only well-formed repetition counts", "[grammar-check]")
{
    // The counts the builder writes: {m}, {m,} and {m,n}.
    CHECK(grammar::Check("root ::= \"a\"{2}\n", "aa").empty());
    CHECK(grammar::Check("root ::= \"a\"{0}\n", "").empty());
    CHECK(grammar::Check("root ::= \"a\"{1,}\n", "aaa").empty());
    CHECK(grammar::Check("root ::= \"a\"{1,3}\n", "aaa").empty());
    CHECK_FALSE(grammar::Check("root ::= \"a\"{1,3}\n", "aaaa").empty());

    // Anything else cannot be read: no digits, a sign, a space, a third part, a least above the most, too many digits.
    for (const std::string counts : {"{}", "{,}", "{,2}", "{a}", "{1,a}", "{-1}", "{+1}", "{ 1}", "{1 }", "{1, 2}",
                                     "{1,2,3}", "{3,2}", "{1234567890}"}) {
        INFO(counts);
        CHECK_THROWS_AS(grammar::Check("root ::= \"a\"" + counts + "\n", "a"), core::ContractError);
    }
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\"{1\n", "a"), core::ContractError); // an unclosed count
}

TEST_CASE("grammar::Check stays fast on nested repetitions over a long text", "[grammar-check]")
{
    // Words of a's, each readable as any mix of "a" and "aa", with optional spaces: without memoisation every way to
    // split every word would be tried again at every repetition count of the outer loop.
    const std::string words = "root ::= (word \" \"?)*\nword ::= (\"a\" | \"aa\")+\n";
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "aaaaaaaaa "; // 2,000 characters in all.
    }
    const auto start = std::chrono::steady_clock::now();
    CHECK(grammar::Check(words, text).empty());
    CHECK(grammar::Check(words, text + "b") == "the grammar rejects 'b' after '..." + text.substr(text.size() - 60) + "'");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/Check.h': No such file or directory` in `tests/grammar/CheckTests.cpp`.

- [ ] **Step 3: Write grammar::Check**

Create `src/grammar/Check.h`:

```cpp
#pragma once

#include <string>
#include <string_view>

namespace morpheus::grammar {

// Whether `text` is accepted by `grammar` (GBNF, root rule "root") from start to end, matched by Unicode code point
// as llama.cpp's grammar sampler does. Returns "" when it is; otherwise "the grammar rejects '<c>' at the start",
// "the grammar rejects '<c>' after '<text>'" (the text cut to its last 60 bytes, with "..." in front when cut), or
// "the text ends before the grammar does". Throws ContractError for a grammar it cannot read.
//
// It reads the GBNF that GrammarBuilder and Morpheus's grammars write, one rule per line ("name ::= body"): string
// literals ("..." with the escapes \" \\ \n \r \t \[ \] \xHH \uHHHH \UHHHHHHHH), character classes ([...] and [^...],
// with ranges and the same escapes), rule references, groups, "|" and the postfixes ?, *, +, {m}, {m,} and {m,n}
// (digits only, m <= n). <c> is the code point at the furthest position any partial match reached; a byte of `text`
// that does not start a UTF-8 character is a character nothing matches. A rule that comes back to itself without
// consuming text is a ContractError when it is matched. It does not model llama.cpp's load-time refusals (left
// recursion at load, the repetition limits): the builder and FromSchema stay within them, and the [llama] tests are
// the authority on them.
std::string Check(const std::string& grammar, std::string_view text);

} // namespace morpheus::grammar
```

Create `src/grammar/Check.cpp`:

```cpp
#include "grammar/Check.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"

namespace morpheus::grammar {

namespace {

// How much of the text before a rejected character a reason quotes, in bytes (as LlamaCppBackend::CheckGrammar).
constexpr std::size_t REASON_CONTEXT_BYTES = 60;
// A repetition's most when it has no upper bound ("*", "+" and "{m,}").
constexpr std::size_t NO_LIMIT = static_cast<std::size_t>(-1);
// The most digits a repetition count may have: more could overflow, and llama.cpp refuses far smaller counts.
constexpr std::size_t MAX_COUNT_DIGITS = 9;
// Stands for a byte of the text that does not start a UTF-8 character: no literal or class matches it.
constexpr char32_t NOT_UTF8 = 0xFFFFFFFF;

// One parsed expression. A rule's body is a Choice. A Reference points at the body of the rule it names once every
// rule is read.
struct Node {
    enum class Kind { Literal, Class, Reference, Sequence, Choice, Repeat };

    Kind kind = Kind::Sequence;
    std::u32string text = {};                                // Literal: the code points to match.
    std::vector<std::pair<char32_t, char32_t>> ranges = {}; // Class: inclusive code point ranges.
    bool negated = false;                                    // Class: [^...].
    std::string rule = {};                                   // Reference: the rule's name.
    const Node* pBody = nullptr;                             // Reference: that rule's body. Not owned.
    std::vector<Node> children = {};                         // Sequence, Choice: the parts; Repeat: the one item.
    std::size_t fewest = 0;                                  // Repeat: the least repetitions.
    std::size_t most = 0;                                    // Repeat: the most repetitions, or NO_LIMIT.
};

// Throws the ContractError for a grammar Check cannot read, naming the problem and the line.
[[noreturn]] void ReadError(std::string_view problem, const std::string& line)
{
    throw core::ContractError(std::format("grammar::Check: {} in: {}", problem, line));
}

// Decodes the UTF-8 character at `pos` and moves past it; false, leaving `pos` as it was, for bytes that are not
// UTF-8.
bool DecodeUtf8(std::string_view text, std::size_t& pos, char32_t& codePoint)
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

// One character of a literal or a class at `pos` of `line`: an escape, or one UTF-8 code point. Moves past it.
char32_t ParseChar(const std::string& line, std::size_t& pos)
{
    if (line[pos] != '\\') {
        char32_t codePoint = 0;
        if (!DecodeUtf8(line, pos, codePoint)) {
            ReadError("invalid UTF-8", line);
        }
        return codePoint;
    }
    if (pos + 1 >= line.size()) {
        ReadError("an escape at the end", line);
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
        ReadError(std::format("an unknown escape '\\{}'", escape), line);
    }
    if (pos + digits > line.size()) {
        ReadError("a short hex escape", line);
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
            ReadError("a bad hex digit", line);
        }
        value = value * 16 + digit;
    }
    pos += digits;
    return static_cast<char32_t>(value);
}

Node ParseChoice(const std::string& line, std::size_t& pos);

// A string literal, a character class, a group or a rule reference at `pos` of `line`. Recursive through groups.
Node ParsePrimary(const std::string& line, std::size_t& pos)
{
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    Node node;
    if (line[pos] == '"') {
        node.kind = Node::Kind::Literal;
        ++pos;
        while (pos < line.size() && line[pos] != '"') {
            node.text += ParseChar(line, pos);
        }
        if (pos >= line.size()) {
            ReadError("an unclosed string literal", line);
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
            ReadError("an unclosed character class", line);
        }
        ++pos;
    } else if (line[pos] == '(') {
        ++pos;
        node = ParseChoice(line, pos);
        if (pos >= line.size() || line[pos] != ')') {
            ReadError("an unclosed '('", line);
        }
        ++pos;
    } else if (isNameChar(line[pos])) {
        node.kind = Node::Kind::Reference;
        while (pos < line.size() && isNameChar(line[pos])) {
            node.rule += line[pos++];
        }
    } else {
        ReadError(std::format("an unexpected '{}'", line[pos]), line);
    }
    return node;
}

// Items, each with its postfixes, up to a "|", a ")" or the end of `line`. Recursive through groups.
Node ParseSequence(const std::string& line, std::size_t& pos)
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
                // {m}, {m,} or {m,n}: decimal digits only, m <= n. Anything else is not a count the builder writes.
                const std::size_t close = line.find('}', pos);
                if (close == std::string::npos) {
                    ReadError("an unclosed '{'", line);
                }
                const std::string counts = line.substr(pos, close - pos);
                pos = close + 1;
                // One count: 1 to MAX_COUNT_DIGITS decimal digits.
                auto readCount = [&](std::string_view digits) -> std::size_t {
                    const bool allDigits =
                        std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
                    if (digits.empty() || digits.size() > MAX_COUNT_DIGITS || !allDigits) {
                        ReadError(std::format("a repetition count '{{{}}}' that is not m, m, or m,n", counts), line);
                    }
                    std::size_t value = 0;
                    for (const char c : digits) {
                        value = value * 10 + static_cast<std::size_t>(c - '0');
                    }
                    return value;
                };
                const std::size_t comma = counts.find(',');
                repeat.fewest = readCount(std::string_view(counts).substr(0, comma));
                if (comma == std::string::npos) {
                    repeat.most = repeat.fewest;
                } else if (comma + 1 == counts.size()) {
                    repeat.most = NO_LIMIT;
                } else {
                    repeat.most = readCount(std::string_view(counts).substr(comma + 1));
                }
                if (repeat.most < repeat.fewest) {
                    ReadError(std::format("a repetition count '{{{}}}' whose least is above its most", counts), line);
                }
            }
            repeat.children.push_back(std::move(item));
            item = std::move(repeat);
        }
        sequence.children.push_back(std::move(item));
    }
}

// Alternatives separated by "|", up to a ")" or the end of `line`. Recursive through groups.
Node ParseChoice(const std::string& line, std::size_t& pos)
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

// Points every reference inside `node` (part of the rule `name`) at the body of the rule it names. Throws
// ContractError for a rule that does not exist. Recursive over the tree.
void Link(Node& node, const std::map<std::string, Node>& rules, const std::string& name)
{
    if (node.kind == Node::Kind::Reference) {
        const auto found = rules.find(node.rule);
        if (found == rules.end()) {
            throw core::ContractError(
                std::format("grammar::Check: '{}' used by '{}' is not defined", node.rule, name));
        }
        node.pBody = &found->second;
    }
    for (Node& child : node.children) {
        Link(child, rules, name);
    }
}

// Matches a read grammar against one text's code points by sets of end positions: every place an expression can
// end when it starts at a position. Each (node, position) is matched once and remembered, so a long text and nested
// repetitions stay polynomial. Tracks the furthest position any literal or class reached: where the text stops
// fitting when the root does not match all of it.
class Matcher {
private:
    const std::u32string* _pText = nullptr; // Not owned.
    std::map<std::pair<const Node*, std::size_t>, std::set<std::size_t>> _memo = {};
    std::set<std::pair<const Node*, std::size_t>> _active = {}; // Rule bodies being matched, with their start.
    std::size_t _furthest = 0;

public:
    explicit Matcher(const std::u32string& text)
        : _pText(&text)
    {
    }

    std::size_t Furthest() const
    {
        return _furthest;
    }

    // Where `node` can end when it starts at `pos`. Throws ContractError for a rule that comes back to itself
    // without consuming text, which would never end. Recursive over the grammar.
    std::set<std::size_t> Ends(const Node& node, std::size_t pos)
    {
        const std::u32string& text = *_pText;

        // Literals and classes match at once; they are what moves the furthest position.
        if (node.kind == Node::Kind::Literal) {
            std::size_t matched = 0;
            while (matched < node.text.size() && pos + matched < text.size() &&
                   text[pos + matched] == node.text[matched]) {
                ++matched;
            }
            _furthest = std::max(_furthest, pos + matched);
            return matched == node.text.size() ? std::set<std::size_t>{pos + matched} : std::set<std::size_t>{};
        }
        if (node.kind == Node::Kind::Class) {
            if (pos >= text.size() || text[pos] == NOT_UTF8) {
                return {};
            }
            const char32_t c = text[pos];
            const bool inRanges = std::any_of(node.ranges.begin(), node.ranges.end(),
                                              [c](const auto& range) { return range.first <= c && c <= range.second; });
            if (inRanges == node.negated) {
                return {};
            }
            _furthest = std::max(_furthest, pos + 1);
            return {pos + 1};
        }

        // Everything else is matched once per position and remembered.
        const std::pair<const Node*, std::size_t> key(&node, pos);
        if (const auto known = _memo.find(key); known != _memo.end()) {
            return known->second;
        }
        std::set<std::size_t> ends;
        switch (node.kind) {
        case Node::Kind::Reference: {
            // A rule body entered again at the same position before it finished would loop forever.
            const std::pair<const Node*, std::size_t> call(node.pBody, pos);
            if (!_active.insert(call).second) {
                throw core::ContractError(
                    std::format("grammar::Check: '{}' comes back to itself without consuming text", node.rule));
            }
            ends = Ends(*node.pBody, pos);
            _active.erase(call);
            break;
        }
        case Node::Kind::Sequence: {
            // Each part starts wherever the parts before it can end.
            ends.insert(pos);
            for (const Node& child : node.children) {
                std::set<std::size_t> next;
                for (const std::size_t start : ends) {
                    const std::set<std::size_t> more = Ends(child, start);
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
                const std::set<std::size_t> more = Ends(child, pos);
                ends.insert(more.begin(), more.end());
            }
            break;
        case Node::Kind::Repeat: {
            // `current` holds where exactly `count` repetitions can end. With no upper bound, a position already
            // reached at a count of at least `fewest` has nothing new to offer and is not followed again. A step that
            // gives back the set it started from (the item matched only empty text) would repeat forever: its set is
            // final, and it counts for `fewest` too, which is within `most`.
            std::set<std::size_t> current = {pos};
            if (node.fewest == 0) {
                ends.insert(pos);
            }
            for (std::size_t count = 1; !current.empty() && (node.most == NO_LIMIT || count <= node.most); ++count) {
                std::set<std::size_t> next;
                for (const std::size_t start : current) {
                    const std::set<std::size_t> more = Ends(node.children[0], start);
                    next.insert(more.begin(), more.end());
                }
                const bool stuck = next == current;
                if (node.most == NO_LIMIT && count >= node.fewest) {
                    std::erase_if(next, [&ends](std::size_t end) { return ends.contains(end); });
                }
                if (count >= node.fewest || stuck) {
                    ends.insert(next.begin(), next.end());
                }
                if (stuck) {
                    break;
                }
                current = std::move(next);
            }
            break;
        }
        case Node::Kind::Literal:
        case Node::Kind::Class:
            break; // Matched above.
        }
        _memo.emplace(key, ends);
        return ends;
    }
};

} // namespace

std::string Check(const std::string& grammar, std::string_view text)
{
    // Read the rules, one per line ("name ::= body"); blank lines are skipped.
    std::map<std::string, Node> rules;
    for (std::size_t start = 0; start < grammar.size();) {
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
            ReadError("a line without ' ::= '", line);
        }
        std::size_t pos = arrow + 5;
        Node body = ParseChoice(line, pos);
        // ParseChoice stops at a ")" it did not open, or at the end of the line.
        if (pos != line.size()) {
            ReadError("an unmatched ')'", line);
        }
        const std::string name = line.substr(0, arrow);
        if (!rules.emplace(name, std::move(body)).second) {
            ReadError(std::format("'{}' defined twice", name), line);
        }
    }
    if (!rules.contains("root")) {
        throw core::ContractError("grammar::Check: the grammar has no root rule");
    }
    // Every reference points at its rule's body; the map's nodes never move from here on.
    for (auto& [name, body] : rules) {
        Link(body, rules, name);
    }

    // The text's code points, and where each starts in bytes (plus the end), so a reason can quote the text. A byte
    // that does not start a UTF-8 character becomes NOT_UTF8, which nothing matches, and decoding goes on after it.
    std::u32string codePoints;
    std::vector<std::size_t> offsets;
    for (std::size_t pos = 0; pos < text.size();) {
        offsets.push_back(pos);
        char32_t codePoint = 0;
        if (DecodeUtf8(text, pos, codePoint)) {
            codePoints += codePoint;
        } else {
            codePoints += NOT_UTF8;
            ++pos;
        }
    }
    offsets.push_back(text.size());

    Matcher matcher(codePoints);
    if (matcher.Ends(rules.at("root"), 0).contains(codePoints.size())) {
        return {};
    }

    // Where the text stops fitting: the furthest position any partial match reached. At the end, every path wanted
    // more text.
    const std::size_t furthest = matcher.Furthest();
    if (furthest >= codePoints.size()) {
        return "the text ends before the grammar does";
    }
    const std::string_view rejected = text.substr(offsets[furthest], offsets[furthest + 1] - offsets[furthest]);
    if (furthest == 0) {
        return std::format("the grammar rejects '{}' at the start", rejected);
    }
    // The text before it: its last REASON_CONTEXT_BYTES bytes, starting on a whole UTF-8 character.
    const std::string_view before = text.substr(0, offsets[furthest]);
    std::size_t cut = before.size() > REASON_CONTEXT_BYTES ? before.size() - REASON_CONTEXT_BYTES : 0;
    while (cut < before.size() && (static_cast<unsigned char>(before[cut]) & 0xC0) == 0x80) {
        ++cut; // A continuation byte: start at the next character.
    }
    return std::format("the grammar rejects '{}' after '{}{}'", rejected, cut > 0 ? "..." : "", before.substr(cut));
}

} // namespace morpheus::grammar
```

(`ParseChar`, `ParsePrimary`, `ParseSequence` and `ParseChoice` are mutually recursive through groups, and `ParseChar` has three call sites; `Link` and `Matcher::Ends` recurse over the tree; `ReadError` and `DecodeUtf8` have several callers.)

In `src/grammar/CMakeLists.txt`, replace

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
    Schema.cpp
)
```

with

```cmake
add_library(morpheus_grammar STATIC
    Check.cpp
    Gbnf.cpp
    Schema.cpp
)
```

- [ ] **Step 4: Run the new tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar-check]"`
Expected: `All tests passed` (8 test cases).

- [ ] **Step 5: Switch every matcher user to grammar::Check and delete the matcher**

The conversion is mechanical, in each file below:
- the include `#include "grammar/GbnfMatcher.h"` becomes `#include "grammar/Check.h"` (keep the include block sorted: `grammar/Check.h` sorts before `grammar/Gbnf.h`);
- a matcher `const test::GbnfMatcher NAME(EXPRESSION);` becomes `const std::string NAME = EXPRESSION;`, keeping its name;
- `NAME.Accepts(TEXT)` becomes `grammar::Check(NAME, TEXT).empty()` (inside `CHECK(...)` and `CHECK_FALSE(...)` alike, so `CHECK_FALSE(NAME.Accepts(TEXT))` becomes `CHECK_FALSE(grammar::Check(NAME, TEXT).empty())`).

Files and their matchers:
- `tests/grammar/GbnfTests.cpp`: the helper becomes

  ```cpp
  // The grammar whose root is `rootBody`.
  std::string GrammarFor(const std::string& rootBody)
  {
      return grammar::GrammarBuilder().Text(rootBody);
  }
  ```

  and each `const test::GbnfMatcher NAME = MatcherFor(EXPRESSION);` becomes `const std::string NAME = GrammarFor(EXPRESSION);` (the matchers `text`, `mood`, `list`, `object`, `either`, `matcher` in the brute-force test, `all`, `top`, `bottom`, `lowest`).
- `tests/grammar/SchemaTests.cpp`: the two `matcher`s.
- `tests/grammar/PropertyTests.cpp`: `CheckRandomValues` becomes

  ```cpp
  void CheckRandomValues(const nlohmann::json& schema)
  {
      const std::string gbnf = grammar::FromSchema(schema);
      std::mt19937_64 random(test::PROPERTY_SEED);
      for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
          const std::string text = test::RandomValue(schema, random).dump();
          INFO("value " << i << ": " << text);
          const std::vector<grammar::ValidationError> errors = grammar::Validate(nlohmann::json::parse(text), schema);
          for (const grammar::ValidationError& error : errors) {
              UNSCOPED_INFO(error.path << ": " << error.message);
          }
          CHECK(errors.empty());
          CHECK(grammar::Check(gbnf, text) == "");
      }
  }
  ```

- `tests/turn/ResolveGrammarTests.cpp`: the matchers `engine` (twice), `assisted`, `lookup`, `matcher` (five times), `both`, `check`, `digest`, `creates` (twice). The comment above the characterization tests ("checked with the test matcher") becomes "checked with grammar::Check". The test cases whose locals are named `grammar` (lines 190-350) use no matcher and need no change.

Delete the matcher and its tests (their cases are now `CheckTests.cpp`'s first three):

```bash
git rm -q tests/grammar/GbnfMatcher.h tests/grammar/GbnfMatcherTests.cpp
```

Then in `tests/llama/LlamaGrammarTests.cpp` (dev build only), make the model-backed tests compare `grammar::Check` with llama.cpp. Replace `#include "grammar/GbnfMatcher.h"` with `#include "grammar/Check.h"`, and after `using Catch::Matchers::ContainsSubstring;` add:

```cpp

namespace {

// The kind of a grammar check's verdict: "fits", "rejects" or "ends early". llama.cpp's reason quotes a token and
// grammar::Check's a code point, so the two agree on the kind, not the quote.
std::string VerdictOf(const std::string& reason)
{
    if (reason.empty()) {
        return "fits";
    }
    if (reason == "the text ends before the grammar does") {
        return "ends early";
    }
    REQUIRE(reason.starts_with("the grammar rejects '"));
    return "rejects";
}

} // namespace
```

Replace the test case "llama.cpp accepts the random values of each test schema against the schema's grammar" with:

```cpp
TEST_CASE("llama.cpp and grammar::Check accept the random values of each test schema", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<nlohmann::json> schemas = {test::CatalogSchema(), test::EveryKeywordSchema()};
    for (const nlohmann::json& schema : schemas) {
        const std::string gbnf = grammar::FromSchema(schema);
        // The grammar parses (one that does not is a ContractError) and wants a whole value, by both checkers.
        CHECK(backend.CheckGrammar(gbnf, "") == "the text ends before the grammar does");
        CHECK(grammar::Check(gbnf, "") == "the text ends before the grammar does");
        // The same values as the fast suite's property test: the same seed, the same draws, written compact.
        std::mt19937_64 random(test::PROPERTY_SEED);
        for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
            const std::string text = test::RandomValue(schema, random).dump();
            INFO("value " << i << ": " << text);
            CHECK(backend.CheckGrammar(gbnf, text) == "");
            CHECK(grammar::Check(gbnf, text) == "");
        }
    }
}
```

In the test case "llama.cpp rejects invalid actions against the catalog grammar", replace its loop with:

```cpp
    for (const std::string& text : invalid) {
        INFO(text);
        const std::string reason = backend.CheckGrammar(gbnf, text);
        CHECK(reason != "");
        CHECK(VerdictOf(grammar::Check(gbnf, text)) == VerdictOf(reason));
    }
```

Replace the test case "llama.cpp and the test matcher agree on IntRange grammars" with:

```cpp
TEST_CASE("llama.cpp and grammar::Check agree on IntRange grammars", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<std::pair<long long, long long>> ranges = {{-15, 123}, {0, 255}, {-1000, -91}, {37, 120}};
    for (const auto& [low, high] : ranges) {
        const std::string gbnf = grammar::GrammarBuilder().Text(grammar::IntRange(low, high));
        // Other spellings, then a window around each end.
        std::vector<std::pair<std::string, bool>> probes = {{"-0", false}, {"00", false}, {"007", false}, {"-01", false},
                                                            {"+5", false}};
        for (long long value = low - 12; value <= low + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (long long value = high - 12; value <= high + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (const auto& [text, inRange] : probes) {
            INFO("IntRange(" << low << ", " << high << ") on '" << text << "'");
            const std::string llamaReason = backend.CheckGrammar(gbnf, text);
            const std::string checkReason = grammar::Check(gbnf, text);
            CHECK(llamaReason.empty() == inRange);
            CHECK(checkReason.empty() == inRange);
            CHECK(VerdictOf(checkReason) == VerdictOf(llamaReason));
        }
    }
}
```

Check that nothing refers to the matcher any more: `grep -rn "GbnfMatcher\|\.Accepts(" tests src` prints nothing.

- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: builds with no warnings; `100% tests passed, 0 tests failed out of 375` (370, minus the matcher's 3, plus `CheckTests.cpp`'s 8).

- [ ] **Step 7: Build the dev targets and run the model-backed grammar tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run (Git Bash, from the repo root):

```bash
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*" "LlamaCppBackend::CheckGrammar*"
```

Expected: `All tests passed`, none skipped. A verdict-kind mismatch in the IntRange test is a real disagreement between `grammar::Check` and llama.cpp: report the probe and both reasons rather than loosening the check.

- [ ] **Step 8: Byte check and commit**

```bash
check_bytes src/grammar/Check.h src/grammar/Check.cpp src/grammar/CMakeLists.txt tests/grammar/CheckTests.cpp tests/grammar/GbnfTests.cpp tests/grammar/SchemaTests.cpp tests/grammar/PropertyTests.cpp tests/turn/ResolveGrammarTests.cpp tests/llama/LlamaGrammarTests.cpp
git add src/grammar/Check.h src/grammar/Check.cpp src/grammar/CMakeLists.txt tests/grammar/CheckTests.cpp tests/grammar/GbnfMatcher.h tests/grammar/GbnfMatcherTests.cpp tests/grammar/GbnfTests.cpp tests/grammar/SchemaTests.cpp tests/grammar/PropertyTests.cpp tests/turn/ResolveGrammarTests.cpp tests/llama/LlamaGrammarTests.cpp
git commit -m "Promote the GBNF matcher into morpheus_grammar as grammar::Check" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Expected: `check_bytes` prints nothing (the `\x` and `\u` escapes in `CheckTests.cpp` must still be escapes).

---

