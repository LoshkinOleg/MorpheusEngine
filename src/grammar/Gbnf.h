#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morpheus::grammar {

// GBNF text fragments for llama.cpp grammars of compact JSON: no optional whitespace, so the model's answer is one
// line. Every function returns an expression that can stand anywhere in a GBNF sequence: one made of alternatives
// comes in parentheses. A broken contract (for example min > max) is a core::ContractError.

// llama.cpp refuses to load a grammar past either of two checks in llama-grammar.cpp, both against
// MAX_REPETITION_THRESHOLD (2000): a repetition count {m,n} above 2000 (parse_sequence), and a repetition whose count
// times the symbols its repeated item creates reaches 2000 (handle_repetitions). A rule reference or a literal is one
// symbol; a parenthesized group is its own rule, plus every rule a repetition inside it creates, plus every rule name
// first seen inside it. The limits below keep every grammar this library writes within both checks.
// JsonString repeats the one reference `char` up to maxChars times: maxChars * 1 < 2000.
constexpr int GBNF_MAX_STRING_CHARS = 1999;
// ListOf repeats the group ("," element) up to maxItems - 1 times. It always writes the element before the repeated
// group, so the group is one symbol; the factor 2 in (maxItems - 1) * 2 < 2000 is a deliberate safety margin.
constexpr int GBNF_MAX_LIST_ITEMS = 1000;

// One key of an Object and the expression for its value.
struct ObjectField {
    std::string key = {};   // The key; Object writes it as a JSON string.
    std::string value = {}; // A GBNF expression for the value.
};

// `text` exactly: a GBNF string literal, with `"` and `\` escaped. A grammar is one rule per line, so `text` must not
// hold a line break. Values that sit inside JSON strings are JSON-escaped by the caller first.
std::string Literal(std::string_view text);

// A JSON string of minChars to maxChars characters. Each character is a code point other than `"`, `\`, U+007F and
// the control characters, or one JSON escape: the shared terminal `char`. Requires 0 <= minChars <= maxChars and
// maxChars <= GBNF_MAX_STRING_CHARS.
std::string JsonString(int minChars, int maxChars);

// The decimal JSON integers from min to max, both included, exactly: no leading zeros, no "-0", no "+". Any range of
// long long with min <= max. Its digit classes are written inline: it needs no shared terminal.
std::string IntRange(long long min, long long max);

// One of `values`, each written as a JSON string. Requires at least one value and no duplicates.
std::string Enum(const std::vector<std::string>& values);

// A JSON array of minItems to maxItems `element`s, compact. Requires 0 <= minItems <= maxItems, maxItems >= 1 and
// maxItems <= GBNF_MAX_LIST_ITEMS. `element` must be a rule reference ([a-z0-9-]+) or one string literal (Literal's
// output): anything larger in the repeated group would break llama.cpp's limit. Name a larger element with
// GrammarBuilder::Rule.
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
    // shared terminals the root and the rules use (`char`). Every rule name the root and the rules use must be
    // one added here or a shared terminal; an undefined one, or a root body that is empty or holds a line break, is a
    // ContractError.
    std::string Text(std::string_view rootBody) const;
};

} // namespace morpheus::grammar
