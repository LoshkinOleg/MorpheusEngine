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
