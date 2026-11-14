#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace morpheus::server {

// Turns a line streamed in pieces into its sentences as soon as each is complete (morpheus_server spec, section 1,
// "Sentences"), so the host's text-to-speech can start on the first one. A sentence ends at '.', '!', '?' or the
// ellipsis character (U+2026), followed by any closing quotes or brackets, then whitespace; the sentence is the text up
// to the closing characters, trimmed. A '.' between digits ends nothing (no whitespace follows it). Pieces may split
// anywhere, inside a word or a UTF-8 character included. Abbreviations ("Mr. Smith") are not special: they end a
// sentence.
class SentenceSplitter {
private:
    std::string _pending = {}; // The text not yet part of a finished sentence.
    std::size_t _scanned = 0;  // How much of _pending has been looked at for a sentence end.

public:
    // Adds the next piece; returns the sentences it completed, in order.
    std::vector<std::string> Add(std::string_view piece);

    // Ends the line: returns what is left, trimmed (the last sentence, or "" when nothing is left), and starts afresh.
    std::string Finish();
};

} // namespace morpheus::server
