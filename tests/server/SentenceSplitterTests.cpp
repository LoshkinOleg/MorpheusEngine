#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "server/SentenceSplitter.h"

using namespace morpheus;

namespace {

// The sentences of `pieces` streamed in order: those Add completes, then what Finish returns when it is not empty.
std::vector<std::string> Split(const std::vector<std::string>& pieces)
{
    server::SentenceSplitter splitter;
    std::vector<std::string> sentences;
    for (const std::string& piece : pieces) {
        for (std::string& sentence : splitter.Add(piece)) {
            sentences.push_back(std::move(sentence));
        }
    }
    const std::string rest = splitter.Finish();
    if (!rest.empty()) {
        sentences.push_back(rest);
    }
    return sentences;
}

// The ellipsis character, U+2026, and the right double and single quotation marks, U+201D and U+2019, in UTF-8.
const std::string ELLIPSIS = "\xE2\x80\xA6";
const std::string RIGHT_DOUBLE = "\xE2\x80\x9D";
const std::string RIGHT_SINGLE = "\xE2\x80\x99";
const std::string LEFT_DOUBLE = "\xE2\x80\x9C";

} // namespace

TEST_CASE("SentenceSplitter ends a sentence at . ! ? and the ellipsis character, then whitespace",
          "[hosted-server][sentences]")
{
    CHECK(Split({"Lead the way. I will follow! Will you? Wait" + ELLIPSIS + " now."}) ==
          std::vector<std::string>{"Lead the way.", "I will follow!", "Will you?", "Wait" + ELLIPSIS, "now."});
}

TEST_CASE("SentenceSplitter keeps closing quotes and brackets with their sentence", "[hosted-server][sentences]")
{
    CHECK(Split({"\"Go home.\" She left. (Quietly.) Done"}) ==
          std::vector<std::string>{"\"Go home.\"", "She left.", "(Quietly.)", "Done"});
    CHECK(Split({LEFT_DOUBLE + "Run!" + RIGHT_DOUBLE + " he said. It" + RIGHT_SINGLE + "s late."}) ==
          std::vector<std::string>{LEFT_DOUBLE + "Run!" + RIGHT_DOUBLE, "he said.", "It" + RIGHT_SINGLE + "s late."});
}

TEST_CASE("SentenceSplitter takes pieces split anywhere, inside a word or a UTF-8 character", "[hosted-server][sentences]")
{
    CHECK(Split({"Hel", "lo", ".", " ", "Th", "ere\xE2", "\x80", "\xA6 ", "end"}) ==
          std::vector<std::string>{"Hello.", "There" + ELLIPSIS, "end"});
}

TEST_CASE("SentenceSplitter gives a sentence as soon as the whitespace after it arrives", "[hosted-server][sentences]")
{
    server::SentenceSplitter splitter;
    CHECK(splitter.Add("Hello.").empty()); // "Hello.5" would not end there: it waits for what follows.
    CHECK(splitter.Add(" Next") == std::vector<std::string>{"Hello."});
    CHECK(splitter.Finish() == "Next");
    // Finish starts it afresh.
    CHECK(splitter.Add("Again. ") == std::vector<std::string>{"Again."});
    CHECK(splitter.Finish().empty());
}

TEST_CASE("SentenceSplitter does not end at a decimal point, ends after dots, and splits abbreviations",
          "[hosted-server][sentences]")
{
    CHECK(Split({"It costs 3.5 septims... Fine. Ok"}) ==
          std::vector<std::string>{"It costs 3.5 septims...", "Fine.", "Ok"});
    // Abbreviations are not special (documented).
    CHECK(Split({"Ask Mr. Smith."}) == std::vector<std::string>{"Ask Mr.", "Smith."});
}

TEST_CASE("SentenceSplitter ends at a newline and gives no empty sentence", "[hosted-server][sentences]")
{
    CHECK(Split({"One.\nTwo.\r\n"}) == std::vector<std::string>{"One.", "Two."});
    CHECK(Split({"  ", "\n"}).empty());
    CHECK(Split({". . "}) == std::vector<std::string>{".", "."});
}
