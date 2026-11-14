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
