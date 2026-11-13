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
