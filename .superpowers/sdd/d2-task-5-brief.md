### Task 5: `SentenceSplitter`

**Files:**
- Create: `src/server/SentenceSplitter.h`, `src/server/SentenceSplitter.cpp`
- Modify: `src/server/CMakeLists.txt` (add `SentenceSplitter.cpp`)
- Test: `tests/server/SentenceSplitterTests.cpp`

**Interfaces:**
- Consumes: `core::Trim(std::string_view) -> std::string` (`src/core/Text.h`).
- Produces: `class server::SentenceSplitter` with `std::vector<std::string> Add(std::string_view piece)` and `std::string Finish()`.

- [ ] **Step 1: Write the failing tests**

Create `tests/server/SentenceSplitterTests.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/SentenceSplitter.h'`.

- [ ] **Step 3: Write the splitter**

Create `src/server/SentenceSplitter.h`:

```cpp
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
```

Create `src/server/SentenceSplitter.cpp`:

```cpp
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
```

Add `SentenceSplitter.cpp` to `add_library(morpheus_server_lib STATIC ...)` in `src/server/CMakeLists.txt` (keep the list sorted).

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[sentences]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/server/SentenceSplitter.h src/server/SentenceSplitter.cpp src/server/CMakeLists.txt tests/server/SentenceSplitterTests.cpp
git commit -m "SentenceSplitter: a streamed line's sentences as soon as each ends"
```

---

