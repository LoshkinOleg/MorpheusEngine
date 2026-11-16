#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "core/Errors.h"
#include "grammar/Check.h"

using namespace morpheus;

namespace {

// How long the nested-repetition text may take to check. AddressSanitizer's Debug build runs the matcher about 8x
// slower (0.8 s measured without it, 6.7 s with it), so it gets more room; either limit is far below what an
// unmemoised matcher takes.
#if defined(__SANITIZE_ADDRESS__)
constexpr std::chrono::seconds NESTED_REPETITION_LIMIT = std::chrono::seconds(30);
#else
constexpr std::chrono::seconds NESTED_REPETITION_LIMIT = std::chrono::seconds(5);
#endif

} // namespace

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
    CHECK(std::chrono::steady_clock::now() - start < NESTED_REPETITION_LIMIT);
}
