### Task 6: The compact transcript

Older turns appear in the transcript layer (L3) as their digest. The newest `prose_turns` turns keep their narration, and an older turn without a digest (played with `compact_transcript` off, or whose review failed) keeps it too. User messages are unchanged. The resolve prompt is assembled by `context::AssembleResolvePrompt`, so `ContextInputs` carries the number, which `Session::Inputs` sets; fold calls always render full narration. `ENGINE_RULES` tells the model why older turns look short.

**Files:**
- Modify: `src/context/Transcript.h`, `src/context/Transcript.cpp`, `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`, `src/turn/Folding.cpp`, `src/turn/Session.cpp`, `src/turn/EngineText.h`
- Test: `tests/context/RenderTests.cpp`, `tests/context/PromptBuilderTests.cpp`, `tests/turn/FoldingTests.cpp`, `tests/turn/SessionTests.cpp`

**Interfaces:**
- Consumes: `core::TurnRecord::digest` (Task 2); `core::Settings::compactTranscript`, `proseTurns` (Task 1); `ReviewScript` and `ReviewSettings` in `SessionTests.cpp` (Task 4).
- Produces:
  - `std::vector<inference::Message> context::RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns)` (throws `ContractError` for `proseTurns < 1`).
  - `int context::ContextInputs::proseTurns = std::numeric_limits<int>::max()` (after `transcript`).

- [ ] **Step 1: Write the failing render tests**

In `tests/context/RenderTests.cpp`, add to the includes:

```cpp
#include <cstddef>
#include <format>
#include <limits>
#include <vector>

#include "core/Errors.h"
```

In "RenderTranscript turns each turn into a user and an assistant message", replace the call with:

```cpp
    const std::vector<inference::Message> messages =
        context::RenderTranscript({TurnWithMechanics(), quiet}, std::numeric_limits<int>::max());
```

Inside the anonymous namespace, after `TurnWithMechanics`, add:

```cpp
// Turns 1..count, each with an input, a narration and a digest that name its number.
std::vector<core::TurnRecord> NumberedTurns(int count)
{
    std::vector<core::TurnRecord> turns;
    for (int number = 1; number <= count; ++number) {
        core::TurnRecord turn;
        turn.number = number;
        turn.input = std::format("Input {}.", number);
        turn.narration = std::format("Narration {}.", number);
        turn.digest = std::format("Digest {}.", number);
        turns.push_back(turn);
    }
    return turns;
}
```

Append:

```cpp
TEST_CASE("RenderTranscript shows older turns as their digest", "[context]")
{
    std::vector<core::TurnRecord> turns = NumberedTurns(4);
    turns[1].digest.clear(); // Turn 2 has no digest: played with compact_transcript off, or its review failed.

    // proseTurns 2: turns 3 and 4 keep their narration; turns 1 and 2 are older.
    const std::vector<inference::Message> messages = context::RenderTranscript(turns, 2);
    REQUIRE(messages.size() == 8);
    CHECK(messages[0].content == "Input 1."); // User messages never change.
    CHECK(messages[1].content == "Digest 1.");
    CHECK(messages[3].content == "Narration 2."); // An older turn without a digest keeps its narration.
    CHECK(messages[4].content == "Input 3.");
    CHECK(messages[5].content == "Narration 3.");
    CHECK(messages[7].content == "Narration 4.");

    // Without a limit every turn shows its narration.
    const std::vector<inference::Message> full = context::RenderTranscript(turns, std::numeric_limits<int>::max());
    CHECK(full[1].content == "Narration 1.");
    CHECK_THROWS_AS(context::RenderTranscript(turns, 0), core::ContractError);
}

TEST_CASE("A compact transcript keeps its prefix from one turn to the next", "[context]")
{
    constexpr std::size_t PROSE_TURNS = 2;
    const std::vector<core::TurnRecord> turns = NumberedTurns(6);
    for (std::size_t n = 1; n < turns.size(); ++n) {
        INFO("turns 1.." << n << " and 1.." << n + 1);
        const std::vector<inference::Message> before = context::RenderTranscript(
            std::vector<core::TurnRecord>(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(n)),
            static_cast<int>(PROSE_TURNS));
        const std::vector<inference::Message> after = context::RenderTranscript(
            std::vector<core::TurnRecord>(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(n + 1)),
            static_cast<int>(PROSE_TURNS));
        // Every entry of the turns before turn n + 1 - PROSE_TURNS is the same in both...
        const std::size_t stableTurns = n > PROSE_TURNS ? n - PROSE_TURNS : 0;
        for (std::size_t i = 0; i < 2 * stableTurns; ++i) {
            CHECK(before[i].content == after[i].content);
        }
        // ...and turn n + 1 - PROSE_TURNS itself changes form: narration before, digest after.
        if (n >= PROSE_TURNS) {
            const std::size_t assistant = 2 * (n + 1 - PROSE_TURNS) - 1;
            CHECK(before[assistant].content != after[assistant].content);
        }
    }
}
```

- [ ] **Step 2: Write the failing prompt, fold and session tests**

Append to `tests/context/PromptBuilderTests.cpp`:

```cpp
TEST_CASE("Digests shrink the transcript layer", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    context::ContextInputs inputs = Inputs(world);
    inputs.transcript.clear();
    for (int number = 5; number <= 7; ++number) {
        core::TurnRecord turn;
        turn.number = number;
        turn.input = "I walk on.";
        turn.narration = std::string(400, 'n');
        turn.digest = "The player walked on.";
        inputs.transcript.push_back(turn);
    }
    const context::Assembly full = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
    inputs.proseTurns = 1;
    const context::Assembly compact = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);

    CHECK(compact.tokens.transcript < full.tokens.transcript);
    CHECK(compact.transcriptCost < full.transcriptCost);
    REQUIRE(compact.prompt.messages.size() == 1 + 6 + 1);
    CHECK(compact.prompt.messages[2].content == "The player walked on.");
    CHECK(compact.prompt.messages[4].content == "The player walked on.");
    CHECK(compact.prompt.messages[6].content == std::string(400, 'n')); // The newest turn keeps its narration.
    CHECK(full.prompt.messages[2].content == std::string(400, 'n'));     // The default shows every narration.
}
```

Append to `tests/turn/FoldingTests.cpp`:

```cpp
TEST_CASE("A fold sees the full narration even of turns with a digest", "[folding]")
{
    core::TurnRecord walked = Turn(1, "I walk.", "You walk the long shore road.");
    walked.digest = "The player walked.";
    const inference::Prompt prompt =
        turn::SummarizePrompt({inference::Role::System, "story"}, {walked, Turn(2, "I wait.", "Time passes.")});
    REQUIRE(prompt.messages.size() == 1 + 4 + 1);
    CHECK(prompt.messages[2].content == "You walk the long shore road.");
    CHECK(prompt.messages[4].content == "Time passes.");
}
```

Append to `tests/turn/SessionTests.cpp`:

```cpp
TEST_CASE("With compact_transcript on, older turns reach the prompt as their digest", "[session][review]")
{
    const fs::path file = NewRunFile("session_compact");
    {
        core::Settings settings = ReviewSettings(false, true);
        settings.proseTurns = 1;
        ReviewScript script{MOVE,
                            {"Narration one.", "Narration two.", "Narration three."},
                            {R"({"digest":"Digest one."})", R"({"digest":"Digest two."})", R"({"digest":"Digest three."})"}};
        test::ScriptedBackend backend(16384, script.Responder());
        turn::Session session = turn::Session::Start(MiniWorld(), settings, file, 42, backend);
        for (const char* input : {"I walk.", "I walk on.", "I walk further."}) {
            REQUIRE(session.RunTurn(input, {}, {}));
        }

        // The third turn's resolve prompt: turn 1 as its digest, turn 2 (the newest) as its narration.
        const auto& calls = backend.Calls(); // Per turn: resolve, narrate, review.
        REQUIRE(calls.size() == 9);
        const inference::Prompt& third = calls[6].prompt;
        REQUIRE(third.messages.size() == 1 + 4 + 1);
        CHECK(third.messages[1].content.starts_with("I walk.")); // The input stays.
        CHECK(third.messages[2].content == "Digest one.");
        CHECK(third.messages[4].content == "Narration two.");
        CHECK_THAT(third.messages[0].content, ContainsSubstring("Older turns in the transcript show a short summary"));
        // The history keeps the full narration (the Play window shows it).
        CHECK(session.History().at(0).narration == "Narration one.");
    }

    // With compact_transcript off, the same run renders every turn in full, its stored digests notwithstanding.
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Load(MiniWorld(), TestSettings(), file, backend);
    REQUIRE(session.RunTurn("I stop.", {}, {}));
    const inference::Prompt& fourth = backend.Calls().at(0).prompt;
    REQUIRE(fourth.messages.size() == 1 + 6 + 1);
    CHECK(fourth.messages[2].content == "Narration one.");
    CHECK(fourth.messages[4].content == "Narration two.");
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `RenderTranscript` does not take two arguments, and `proseTurns` is not a member of `morpheus::context::ContextInputs`.

- [ ] **Step 4: Render digests for older turns**

In `src/context/Transcript.h`, replace the `RenderTranscript` comment and declaration with:

```cpp
// L3: each turn becomes a user message (the input, then the mechanics line on its own line when there is one) and
// an assistant message: the narration for the newest `proseTurns` turns; for an older turn its digest when that is
// not empty, else its narration. std::numeric_limits<int>::max() shows every narration (fold calls, and the session
// with compact_transcript off). Throws ContractError for proseTurns < 1.
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns);
```

In `src/context/Transcript.cpp`, add `#include "core/Errors.h"` to the includes, and replace `RenderTranscript` with:

```cpp
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns)
{
    MORPHEUS_REQUIRE(proseTurns >= 1, "RenderTranscript: proseTurns must be at least 1");
    // Turns from this index on are the newest proseTurns; the ones before it are older.
    const std::size_t prose = static_cast<std::size_t>(proseTurns);
    const std::size_t firstProse = turns.size() > prose ? turns.size() - prose : 0;

    std::vector<inference::Message> messages;
    messages.reserve(turns.size() * 2);
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const core::TurnRecord& turn = turns[i];
        const std::string mechanics = RenderMechanicsLine(turn);
        messages.push_back({inference::Role::User, mechanics.empty() ? turn.input : turn.input + "\n" + mechanics});
        // An older turn shows its digest. One without (played with compact_transcript off, or its review failed)
        // keeps its narration.
        const bool digested = i < firstProse && !turn.digest.empty();
        messages.push_back({inference::Role::Assistant, digested ? turn.digest : turn.narration});
    }
    return messages;
}
```

- [ ] **Step 5: Pass the number through the prompt builder, the session and folding**

In `src/context/PromptBuilder.h`, add `#include <limits>` to the includes, and in `ContextInputs` after the `transcript` member add:

```cpp
    // L3: the newest turns that show their narration; older ones show their digest (context::RenderTranscript).
    // The default shows every narration.
    int proseTurns = std::numeric_limits<int>::max();
```

In `src/context/PromptBuilder.cpp`, replace `for (inference::Message& message : RenderTranscript(inputs.transcript)) {` with:

```cpp
    for (inference::Message& message : RenderTranscript(inputs.transcript, inputs.proseTurns)) {
```

In `src/turn/Folding.cpp`, add `#include <limits>` to the includes, and replace the first lines of `SummarizePrompt`'s body with:

```cpp
    // The system message, then the turns as the transcript renders them (stable, so cached), then the instruction.
    // Summaries are written from the full narration, digests or not.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (inference::Message& message : context::RenderTranscript(turns, std::numeric_limits<int>::max())) {
```

In `src/turn/Session.cpp`, add `#include <limits>` to the includes, and in `Session::Inputs` after `inputs.state = state;` add:

```cpp
    // With the compact transcript on, older turns show their digest; otherwise every turn shows its narration.
    inputs.proseTurns = _settings.compactTranscript ? _settings.proseTurns : std::numeric_limits<int>::max();
```

- [ ] **Step 6: Tell the model about digests**

In `src/turn/EngineText.h`, replace the last line of `ENGINE_RULES`

```cpp
In the transcript, the bracketed line after a player's input is the engine's record of that turn: rolls first, then applied changes.)";
```

with

```cpp
In the transcript, the bracketed line after a player's input is the engine's record of that turn: rolls first, then applied changes. Older turns in the transcript show a short summary of what was narrated rather than the narration itself; narrate in full prose regardless.)";
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[context],[folding],[session]"`
Expected: all pass. (`ENGINE_RULES` grows by about 35 FakeBackend tokens; the folding tests' comments say two turns fit the transcript budget and three do not, which still holds.)

Then: `ctest --preset tests`
Expected: all pass, the replays included (their fixtures turn the compact transcript on; summaries are exempt from the replay's checks, so where folds land does not matter).

- [ ] **Step 8: Commit**

```bash
git add src/context/Transcript.h src/context/Transcript.cpp src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/Folding.cpp src/turn/Session.cpp src/turn/EngineText.h tests/context/RenderTests.cpp tests/context/PromptBuilderTests.cpp tests/turn/FoldingTests.cpp tests/turn/SessionTests.cpp
git commit -m "Show older transcript turns as their digest" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

