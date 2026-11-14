#include "server/SentenceSplitter.h"

#include <algorithm>
#include <array>

#include "core/Text.h"

namespace morpheus::server {

namespace {

// What ends a sentence: '.', '!', '?' and the ellipsis character (U+2026 in UTF-8).
constexpr std::array<std::string_view, 4> ENDINGS = {".", "!", "?", "\xE2\x80\xA6"};
// What may close a sentence after its ending: quotes and brackets, ASCII and UTF-8 (U+201D, U+2019, U+00BB).
constexpr std::array<std::string_view, 8> CLOSERS = {"\"", "'", ")", "]", "}", "\xE2\x80\x9D", "\xE2\x80\x99", "\xC2\xBB"};

} // namespace

std::vector<std::string> SentenceSplitter::Add(std::string_view piece)
{
    _pending += piece;
    std::vector<std::string> sentences;

    // Each whitespace not looked at yet may end a sentence: it does when the text before it, once its closing
    // characters are set aside, ends with an ending. The text before a whitespace never changes, so each is looked at
    // once.
    std::size_t at = _scanned;
    while (at < _pending.size()) {
        const char c = _pending[at];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            ++at;
            continue;
        }
        std::string_view before = std::string_view(_pending).substr(0, at);
        for (bool closed = true; closed;) {
            closed = false;
            for (const std::string_view closer : CLOSERS) {
                if (before.ends_with(closer)) {
                    before.remove_suffix(closer.size());
                    closed = true;
                    break;
                }
            }
        }
        const bool ends = std::any_of(ENDINGS.begin(), ENDINGS.end(),
                                      [&before](std::string_view ending) { return before.ends_with(ending); });
        if (!ends) {
            ++at;
            continue;
        }
        // A sentence: everything before the whitespace, trimmed. What follows the whitespace is looked at next.
        std::string sentence = core::Trim(std::string_view(_pending).substr(0, at));
        if (!sentence.empty()) {
            sentences.push_back(std::move(sentence));
        }
        _pending.erase(0, at + 1);
        at = 0;
    }
    _scanned = _pending.size();
    return sentences;
}

std::string SentenceSplitter::Finish()
{
    std::string rest = core::Trim(_pending);
    _pending.clear();
    _scanned = 0;
    return rest;
}

} // namespace morpheus::server
