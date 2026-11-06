### Task 1: Core carry-overs (over-removal, seeds, text helpers, UTF-8 paths)

**Files:**
- Create: `src/core/Text.h`, `src/core/Text.cpp`, `tests/core/TextTests.cpp`
- Modify: `src/core/CMakeLists.txt` (add `Text.cpp`), `src/core/Changes.h`, `src/core/Changes.cpp`, `src/core/StateSchema.cpp`, `src/core/Dice.h`, `src/core/Dice.cpp`, `src/core/Settings.cpp`, `src/core/World.cpp`, `src/store/RunStore.cpp`, `src/store/Sqlite.cpp`, `src/llama/LlamaCppBackend.cpp`
- Modify tests: `tests/core/ChangesTests.cpp`, `tests/core/DiceTests.cpp`

**Interfaces:**
- Consumes: the existing core code.
- Produces:
  - `core::PathText(const std::filesystem::path&) -> std::string` (UTF-8, `/` separators)
  - `core::SHORT_JSON_CHARS` (80) and `core::ShortJson(const nlohmann::json&) -> std::string`
  - `core::CutUtf8(std::string_view, std::size_t maxBytes) -> std::string`
  - `core::Trim(std::string_view) -> std::string`
  - `enum class core::CallKind { Lookup, Resolve, Narrate, Summarize }`
  - `core::SamplingSeed(std::uint64_t runSeed, int turn, CallKind kind, int attempt) -> std::uint32_t`
  - Behavior: `add` with a negative value on a counter item rejects over-removal and notes "none left" at exactly 0.

- [ ] **Step 1: Write the failing tests**

Create `tests/core/TextTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/Text.h"

using namespace morpheus;

TEST_CASE("PathText gives UTF-8 with forward slashes", "[text]")
{
    const std::filesystem::path path = std::filesystem::path(u8"saves") / u8"caf\u00e9" / u8"run.db";
    CHECK(core::PathText(path) == "saves/caf\xC3\xA9/run.db");
}

TEST_CASE("ShortJson is compact ASCII cut at SHORT_JSON_CHARS", "[text]")
{
    CHECK(core::ShortJson(nlohmann::json{{"a", 1}}) == R"({"a":1})");
    CHECK(core::ShortJson(nlohmann::json("caf\xC3\xA9")) == R"("caf\u00e9")");
    const std::string cut = core::ShortJson(nlohmann::json(std::string(500, 'x')));
    CHECK(cut.size() == core::SHORT_JSON_CHARS + 3);
    CHECK(cut.ends_with("..."));
}

TEST_CASE("CutUtf8 never splits a character", "[text]")
{
    CHECK(core::CutUtf8("abc", 10) == "abc");
    CHECK(core::CutUtf8("abcdef", 3) == "abc");
    CHECK(core::CutUtf8("caf\xC3\xA9", 4) == "caf"); // The 2-byte character does not fit whole.
    CHECK(core::CutUtf8("caf\xC3\xA9", 5) == "caf\xC3\xA9");
}

TEST_CASE("Trim removes surrounding ASCII whitespace", "[text]")
{
    CHECK(core::Trim("  a b \r\n\t") == "a b");
    CHECK(core::Trim(" \n ").empty());
}
```

In `tests/core/ChangesTests.cpp`, update the existing "add and remove on counter items" test: the used-up note is now "none left".

```cpp
    CHECK(usedUp.applied.at(0).note == "none left");
```

Then append:

```cpp
TEST_CASE("removing more of a counter item than is held is rejected", "[changes]")
{
    const ApplyResult result = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": -3})");
    REQUIRE(result.rejected.size() == 1);
    CHECK_THAT(result.rejected[0].reason, ContainsSubstring("only 1 'rope' held"));
    CHECK(PlayerField(result, "inventory") == nlohmann::json({{"rope", 1}}));
}

TEST_CASE("rejection reasons cut huge proposed values", "[changes]")
{
    const nlohmann::json change = {{"op", "set"}, {"path", "player.health"}, {"value", std::string(1000, 'x')}};
    const ApplyResult result = ApplyChanges(TestSchema(), TestState(), nlohmann::json::array({change}));
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.rejected[0].reason.size() < 200);
}
```

Append to `tests/core/DiceTests.cpp` (add `#include "core/Errors.h"` if missing):

```cpp
TEST_CASE("SamplingSeed is stable and separates turns, calls and attempts", "[dice]")
{
    const std::uint32_t resolve = SamplingSeed(42, 3, CallKind::Resolve, 0);
    CHECK(SamplingSeed(42, 3, CallKind::Resolve, 0) == resolve);
    CHECK(SamplingSeed(42, 3, CallKind::Narrate, 0) != resolve);
    CHECK(SamplingSeed(42, 3, CallKind::Resolve, 1) != resolve);
    CHECK(SamplingSeed(42, 4, CallKind::Resolve, 0) != resolve);
    CHECK(SamplingSeed(43, 3, CallKind::Resolve, 0) != resolve);
    CHECK(SamplingSeed(42, 0, CallKind::Summarize, 0) == SamplingSeed(42, 0, CallKind::Summarize, 0));
    CHECK_THROWS_AS(SamplingSeed(42, -1, CallKind::Resolve, 0), ContractError);
    CHECK_THROWS_AS(SamplingSeed(42, 1, CallKind::Resolve, -1), ContractError);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `core/Text.h` cannot be opened, and `SamplingSeed` / `CallKind` are undeclared.

- [ ] **Step 3: Implement**

Create `src/core/Text.h`:

```cpp
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace morpheus::core {

// A path as UTF-8 with '/' separators, for messages and for C APIs that take UTF-8. path::string() and
// generic_string() convert through the ANSI code page on Windows, which mangles non-ASCII folder names.
std::string PathText(const std::filesystem::path& path);

// Longest JSON text ShortJson keeps; longer text is cut and ends with "...".
constexpr std::size_t SHORT_JSON_CHARS = 80;

// `value` as compact ASCII JSON (non-ASCII is escaped, so a cut never splits a character), cut to SHORT_JSON_CHARS.
// For rejection reasons, which reach the narrate prompt: a huge proposed value must not flood it.
std::string ShortJson(const nlohmann::json& value);

// The longest prefix of `text` of at most maxBytes bytes that ends on a UTF-8 character boundary.
std::string CutUtf8(std::string_view text, std::size_t maxBytes);

// `text` without leading and trailing spaces, tabs, carriage returns and newlines.
std::string Trim(std::string_view text);

} // namespace morpheus::core
```

Create `src/core/Text.cpp`:

```cpp
#include "core/Text.h"

namespace morpheus::core {

std::string PathText(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string ShortJson(const nlohmann::json& value)
{
    std::string text = value.dump(-1, ' ', true);
    if (text.size() > SHORT_JSON_CHARS) {
        text.resize(SHORT_JSON_CHARS);
        text += "...";
    }
    return text;
}

std::string CutUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    // Back off while the first byte left out is a continuation byte (10xxxxxx): the cut is inside a character.
    std::size_t length = maxBytes;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return std::string(text.substr(0, length));
}

std::string Trim(std::string_view text)
{
    constexpr std::string_view WHITESPACE = " \t\r\n";
    const std::size_t first = text.find_first_not_of(WHITESPACE);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(WHITESPACE);
    return std::string(text.substr(first, last - first + 1));
}

} // namespace morpheus::core
```

Add `Text.cpp` to `add_library(morpheus_core ...)` in `src/core/CMakeLists.txt`.

**Changes.** In `src/core/Changes.cpp`:
1. Include `"core/Text.h"`.
2. Replace each `change.value.dump()` in a rejection reason (three places) with `ShortJson(change.value)`.
3. Replace the counter decrease block, from `if (next <= 0) {` through its `else`, with:

```cpp
                if (next < 0) {
                    return std::format("only {} '{}' held, cannot remove {}", current, item, -delta);
                }
                if (next == 0) {
                    field.erase(item);
                    note = "none left";
                } else {
                    field[item] = next;
                }
```

In `src/core/Changes.h`, update the counter bullet of the `ApplyChanges` comment:

```cpp
//           counter item -> adds a non-zero integer in int range; the count may not exceed INT_MAX (rejected);
//           decreasing below 0 is rejected ("only N 'item' held"); reaching exactly 0 removes the item (note
//           "none left"); decreasing an absent item is rejected.
```

**StateSchema.** In `src/core/StateSchema.cpp`, `CheckFieldValue`, replace every `value.dump()`, `item.dump()` and `count.dump()` with `ShortJson(...)` of the same value, and include `"core/Text.h"`. Leave the `FromJson` bound message, which is world data, unchanged.

**Dice.** In `src/core/Dice.h`, after `CheckSeed`:

```cpp
// Which model call a sampling seed is for.
enum class CallKind { Lookup, Resolve, Narrate, Summarize };

// Sampling seed for one model call, from (runSeed, turn, kind, attempt). A regenerated turn (attempt + 1) samples
// new text while CheckSeed keeps its dice. Throws ContractError unless turn >= 0 and 0 <= attempt < 2^24.
std::uint32_t SamplingSeed(std::uint64_t runSeed, int turn, CallKind kind, int attempt);
```

In `src/core/Dice.cpp`, inside the anonymous namespace (create one if the file has none):

```cpp
// Mixed into the run seed so that sampling seeds and check seeds never coincide for the same turn.
constexpr std::uint64_t SAMPLING_SALT = 0x53414D504C494E47ULL; // "SAMPLING"
```

and after `CheckSeed`:

```cpp
std::uint32_t SamplingSeed(std::uint64_t runSeed, int turn, CallKind kind, int attempt)
{
    MORPHEUS_REQUIRE(turn >= 0 && attempt >= 0 && attempt < (1 << 24),
                     "SamplingSeed: turn must be >= 0 and attempt in 0..2^24-1");
    const std::uint64_t base = SplitMix64(runSeed ^ SAMPLING_SALT).Next();
    // Turn in the high half; kind and attempt in the low half (attempt < 2^24 leaves the kind bits alone).
    SplitMix64 mixer(base ^ ((static_cast<std::uint64_t>(turn) << 32) | (static_cast<std::uint64_t>(kind) << 24) |
                             static_cast<std::uint64_t>(attempt)));
    return static_cast<std::uint32_t>(mixer.Next() >> 32);
}
```

**UTF-8 paths and filesystem errors.** Include `"core/Text.h"` where needed and make these changes:
- `src/core/Settings.cpp`:
  - Every `generic_string()` becomes `PathText(...)`.
  - In `requireModel`, use the non-throwing overload:
    ```cpp
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) {
    ```
    The message stays "does not exist".
- `src/core/World.cpp`:
  - Every `generic_string()` becomes `PathText(...)`.
  - `folderName` becomes `PathText(dir.filename())`.
- `src/store/RunStore.cpp` and `src/store/Sqlite.cpp`: every `generic_string()` becomes `core::PathText(...)`.
- `RunStore::Create` and `RunStore::Open`: use `std::filesystem::exists(file, error)` and `create_directories(..., error)` with a `std::error_code`.
  - If `error` is set, throw `StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()))`.
  - For the folder, throw `StoreError(std::format("cannot create the folder for '{}': {}", ...))`.
  - Keep the existing "already exists" and "does not exist" messages for their cases.
- `src/llama/LlamaCppBackend.cpp`:
  - Delete the `Utf8` helper.
  - `Utf8(path.generic_u8string())` becomes `core::PathText(path)`, and likewise for `_config.embeddingModel`.
  - `EmbeddingModelId()` returns `core::PathText(_config.embeddingModel.stem())`.
  - Model loading keeps passing `path.u8string()` bytes to llama.cpp.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[text],[changes],[dice]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

Then build `dev` to confirm the llama backend compiles: `cmake --build --preset dev` (long; run it in the background or with the max timeout).

- [ ] **Step 5: Commit**

```bash
git add src/core src/store src/llama tests/core
git commit -m "Reject counter over-removal, add sampling seeds and UTF-8 text helpers" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

