### Task 9: Folding calls (summaries)

**Files:**
- Create: `src/turn/Folding.h`, `src/turn/Folding.cpp`, `tests/turn/FoldingTests.cpp`
- Modify: `src/turn/CMakeLists.txt` (add `Folding.cpp`)

**Interfaces:**
- Consumes:
  - `context::RenderTranscript`
  - `SUMMARIZE_INSTRUCTION` and `CONDENSE_INSTRUCTION` (Task 7)
  - `core::Trim`
- Produces:
  - `turn::SummarizeTurns(const inference::Message& systemMessage, const std::vector<core::TurnRecord>& turns, const inference::GenerationSpec& spec, inference::LlmBackend& backend, std::stop_token stop) -> std::optional<core::Summary>`
  - `turn::CondenseSummaries(const inference::Message& systemMessage, const std::vector<core::Summary>& summaries, const inference::GenerationSpec& spec, inference::LlmBackend& backend, std::stop_token stop) -> std::optional<core::Summary>`

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/FoldingTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/Folding.h"

using namespace morpheus;

namespace {

core::TurnRecord Turn(int number, const std::string& input, const std::string& narration)
{
    core::TurnRecord record;
    record.number = number;
    record.input = input;
    record.narration = narration;
    return record;
}

} // namespace

TEST_CASE("SummarizeTurns folds turns after the system message and trims the summary", "[folding]")
{
    test::ScriptedBackend backend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("  The keeper lit the lamp.\n");
    });
    const inference::Message system = {inference::Role::System, "SYSTEM"};
    inference::GenerationSpec spec;
    spec.maxTokens = 64;

    const std::optional<core::Summary> summary = turn::SummarizeTurns(
        system, {Turn(3, "I knock.", "The door creaks."), Turn(4, "I enter.", "It is dark.")}, spec, backend, {});
    REQUIRE(summary);
    CHECK(summary->fromTurn == 3);
    CHECK(summary->toTurn == 4);
    CHECK(summary->text == "The keeper lit the lamp.");

    const inference::Prompt& prompt = backend.Calls().at(0).prompt;
    REQUIRE(prompt.messages.size() == 6); // system, 2 x (user, assistant), instruction
    CHECK(prompt.messages[0].content == "SYSTEM");
    CHECK(prompt.messages[1].content == "I knock.");
    CHECK(prompt.messages[4].content == "It is dark.");
    CHECK(prompt.messages[5].content == turn::SUMMARIZE_INSTRUCTION);
    CHECK(prompt.stableCount == 5);
    CHECK(backend.Calls().at(0).spec.grammar.empty());
}

TEST_CASE("CondenseSummaries covers every summary's range", "[folding]")
{
    test::ScriptedBackend backend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("All of it.");
    });
    const std::optional<core::Summary> merged = turn::CondenseSummaries(
        {inference::Role::System, "SYSTEM"}, {{1, 3, "a"}, {4, 9, "b"}}, inference::GenerationSpec{}, backend, {});
    REQUIRE(merged);
    CHECK(merged->fromTurn == 1);
    CHECK(merged->toTurn == 9);
    CHECK(merged->text == "All of it.");
    const inference::Prompt& prompt = backend.Calls().at(0).prompt;
    REQUIRE(prompt.messages.size() == 2);
    CHECK(prompt.messages[1].content == turn::CONDENSE_INSTRUCTION);
}

TEST_CASE("Folding calls return nothing when cancelled and reject empty input", "[folding]")
{
    test::ScriptedBackend backend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("Summary.");
    });
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(turn::SummarizeTurns({inference::Role::System, "S"}, {Turn(1, "a", "b")}, {}, backend, stop.get_token()));
    CHECK_FALSE(turn::CondenseSummaries({inference::Role::System, "S"}, {{1, 1, "a"}}, {}, backend, stop.get_token()));
    CHECK_THROWS_AS(turn::SummarizeTurns({inference::Role::System, "S"}, {}, {}, backend, {}), core::ContractError);
    CHECK_THROWS_AS(turn::CondenseSummaries({inference::Role::System, "S"}, {}, {}, backend, {}), core::ContractError);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `turn/Folding.h` cannot be opened.

- [ ] **Step 3: Implement**

Create `src/turn/Folding.h`:

```cpp
#pragma once

#include <optional>
#include <stop_token>
#include <vector>

#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Summarizes `turns` (the oldest unfolded turns, in order) with one call: `systemMessage` (the resolve prompt's
// system message, L0-L2), the turns as transcript messages, then SUMMARIZE_INSTRUCTION. That prompt starts like the
// resolve prompt, so the KV cache serves most of it. The summary covers the turns' range; its text is trimmed.
// Returns nullopt when `stop` cancels the call. Throws ContractError for no turns or a non-system message.
std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<core::TurnRecord>& turns,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop);

// Condenses every summary into one covering their whole range: `systemMessage` (whose L2 holds them), then
// CONDENSE_INSTRUCTION. Returns nullopt when cancelled. Throws ContractError for no summaries.
std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop);

} // namespace morpheus::turn
```

Create `src/turn/Folding.cpp`:

```cpp
#include "turn/Folding.h"

#include <string>
#include <utility>

#include "context/Transcript.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

namespace {

// The summary text from a completion: trimmed; a model that wrote nothing still leaves a marker, not a blank.
std::string SummaryText(const std::string& output)
{
    std::string text = core::Trim(output);
    return text.empty() ? std::string("(no summary)") : text;
}

} // namespace

std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<core::TurnRecord>& turns,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !turns.empty(),
                     "SummarizeTurns: needs the system message and at least one turn");
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (inference::Message& message : context::RenderTranscript(turns)) {
        prompt.messages.push_back(std::move(message));
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, std::string(SUMMARIZE_INSTRUCTION)});

    const inference::Completion completion = backend.Complete(prompt, spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }
    return core::Summary{turns.front().number, turns.back().number, SummaryText(completion.text)};
}

std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !summaries.empty(),
                     "CondenseSummaries: needs the system message and at least one summary");
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    prompt.stableCount = 1;
    prompt.messages.push_back({inference::Role::User, std::string(CONDENSE_INSTRUCTION)});

    const inference::Completion completion = backend.Complete(prompt, spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, SummaryText(completion.text)};
}

} // namespace morpheus::turn
```

Add `Folding.cpp` to `src/turn/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[folding]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn tests/turn/FoldingTests.cpp
git commit -m "Add the summarize and condense calls" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
