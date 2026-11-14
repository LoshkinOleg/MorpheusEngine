### Task 3: The shared memory upkeep

`Session`'s fit loop (`AssembleFitting`), `FoldOldest` and `Condense` move into `turn::MemoryUpkeep`, which works on summaries and rendered transcript entries (`context::TranscriptEntry`), not on `core::TurnRecord`. The fold functions (`turn/Folding.h`) and the owned prompt builder take entries too. `Session` changes only by calling the upkeep; its prompts, calls and messages stay byte-identical, so every `Session` test passes unchanged. `FoldOldest` now takes how many entries to fold (the fit loop passes half the transcript as before), so `FoldAll` can fold an ended conversation in one call.

**Files:**
- Modify: `src/context/Transcript.h`, `src/context/Transcript.cpp`
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`
- Modify: `src/turn/Folding.h`, `src/turn/Folding.cpp`
- Create: `src/turn/MemoryUpkeep.h`, `src/turn/MemoryUpkeep.cpp`
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`, `src/turn/CMakeLists.txt`
- Create: `tests/turn/MemoryUpkeepTests.cpp`
- Modify: `tests/context/RenderTests.cpp`, `tests/context/PromptBuilderTests.cpp`, `tests/turn/FoldingTests.cpp`, `tests/app/ModelSeatTests.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 1-2.
- Produces (`context/Transcript.h`): `struct context::TranscriptEntry { int turn = 0; std::vector<inference::Message> messages; std::vector<inference::Message> foldMessages; std::string fact; }`; `std::vector<TranscriptEntry> context::TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns)`.
- Produces (`context/PromptBuilder.h`): `ContextInputs::transcript` is `std::vector<TranscriptEntry>`; `ContextInputs::proseTurns` is gone (the caller renders).
- Produces (`turn/Folding.h`): `SummarizePrompt(const inference::Message&, const std::vector<context::TranscriptEntry>&)`, `SummarizeFallback(const std::vector<context::TranscriptEntry>&, int)`, `SummarizeTurns(const inference::Message&, const std::vector<context::TranscriptEntry>&, const inference::GenerationSpec&, inference::LlmBackend&, std::stop_token)`; the condense functions are unchanged.
- Produces (`turn/MemoryUpkeep.h`): `class turn::TurnError` (moved from `Session.h`); `FOLD_SUMMARY_DIVISOR`, `CONDENSE_SUMMARY_DIVISOR` (moved); `struct turn::FitPass { context::Assembly assembly; std::vector<context::TranscriptEntry> transcript; }`; `class turn::MemoryUpkeep` with `using Assemble = std::function<FitPass(const std::vector<RetrievalHit>& hits)>`, `using StoreFold = std::function<void(const core::Summary& summary)>`, constructor `MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings, std::string tailName, int turn, int attempt, StoreFold storeFold)`, `std::optional<context::Assembly> Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble, std::stop_token stop)`, `bool Settle(const Assemble& assemble, std::stop_token stop)`, `bool FoldAll(const Assemble& assemble, std::stop_token stop)`.
- Produces (`turn/Session.h`): `Session` keeps its public API; `turn/Session.h` includes `turn/MemoryUpkeep.h`.

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/MemoryUpkeepTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "context/Transcript.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "store/RunStore.h"
#include "turn/EngineText.h"
#include "turn/MemoryUpkeep.h"

using namespace morpheus;

namespace {

// Memory calls' replies: a condense call writes "Condensed.", a fold call "Folded.".
test::ScriptedBackend::Responder MemoryResponder()
{
    return [](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        return prompt.messages.back().content == turn::CONDENSE_INSTRUCTION ? std::string("Condensed.")
                                                                             : std::string("Folded.");
    };
}

core::Settings UpkeepSettings()
{
    core::Settings settings;
    settings.budget = {64, 64, 64, 1024, 1024, 1024, 512, 256}; // tail_allowance 1024, summary_cap 512.
    settings.resolveSampling = {0.3, 40, 0.9};
    return settings;
}

// Turn `number` as an entry: L3 shows "Shown N.", a fold reads "Narration N.".
context::TranscriptEntry Entry(int number)
{
    context::TranscriptEntry entry;
    entry.turn = number;
    entry.messages = {{inference::Role::User, std::format("Input {}.", number)},
                      {inference::Role::Assistant, std::format("Shown {}.", number)}};
    entry.foldMessages = {{inference::Role::User, std::format("Input {}.", number)},
                          {inference::Role::Assistant, std::format("Narration {}.", number)}};
    entry.fact = std::format("Input {}.", number);
    return entry;
}

// `count` retrieval hits, h1 to hN.
std::vector<turn::RetrievalHit> Hits(int count)
{
    std::vector<turn::RetrievalHit> hits;
    for (int i = 1; i <= count; ++i) {
        hits.push_back({store::EmbeddingKind::Lore, std::format("h{}", i), std::format("hit {}", i), 1.0f, 1, {}});
    }
    return hits;
}

// A stand-in for a session: `entries` are its turns, and the unfolded ones follow the last summary's range. Its
// prompt is flagged by rules the test sets: the tail is over its allowance while more than `maxHits` hits are left
// (-1: always), the summary over its cap while more than `maxSummaries` summaries are stored, and the transcript over
// its budget while more than `maxEntries` entries are unfolded. Its StoreFold adds the summary and records it.
struct Harness {
    store::RunStore store; // Set by the constructor.
    std::vector<context::TranscriptEntry> entries = {};
    int maxHits = 100;
    std::size_t maxSummaries = 100;
    std::size_t maxEntries = 100;
    std::vector<core::Summary> folded = {};

    Harness(const std::string& name, int firstTurn, int lastTurn)
        : store(store::RunStore::Create(test::FreshTempDir(name) / "run.db", store::RunMeta{},
                                        nlohmann::json::object()))
    {
        for (int number = firstTurn; number <= lastTurn; ++number) {
            entries.push_back(Entry(number));
        }
    }

    turn::MemoryUpkeep Upkeep(test::ScriptedBackend& backend, const core::Settings& settings)
    {
        return turn::MemoryUpkeep(store, backend, settings, "the test tail", 9, 0, [this](const core::Summary& summary) {
            store.AddSummary(summary);
            folded.push_back(summary);
        });
    }

    turn::MemoryUpkeep::Assemble Assemble()
    {
        return [this](const std::vector<turn::RetrievalHit>& hits) {
            const std::vector<core::Summary> summaries = store.Summaries();
            const int foldedThrough = summaries.empty() ? 0 : summaries.back().toTurn;
            turn::FitPass pass;
            for (const context::TranscriptEntry& entry : entries) {
                if (entry.turn > foldedThrough) {
                    pass.transcript.push_back(entry);
                }
            }
            pass.assembly.prompt.messages = {{inference::Role::System, "SYSTEM"}, {inference::Role::User, "TAIL"}};
            pass.assembly.prompt.stableCount = 1;
            pass.assembly.tokens.tail = 7;
            pass.assembly.tailOverBudget = static_cast<int>(hits.size()) > maxHits;
            pass.assembly.summaryOverBudget = summaries.size() > maxSummaries;
            pass.assembly.transcriptOverBudget = pass.transcript.size() > maxEntries;
            return pass;
        };
    }
};

} // namespace

TEST_CASE("Fit sheds hits from the end while the tail is over its allowance, then names the caller's tail", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_tail", 1, 0);
    harness.maxHits = 1;
    std::vector<turn::RetrievalHit> hits = Hits(3);
    const std::optional<context::Assembly> fitted = harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), {});
    REQUIRE(fitted);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].refId == "h1"); // The last hits go first.
    CHECK(backend.Calls().empty());

    // A tail over its allowance with no hit left is a TurnError naming what the caller's tail holds.
    harness.maxHits = -1;
    std::vector<turn::RetrievalHit> more = Hits(2);
    CHECK_THROWS_AS(harness.Upkeep(backend, settings).Fit(more, harness.Assemble(), {}), turn::TurnError);
    CHECK(more.empty());
    CHECK_THROWS_WITH(harness.Upkeep(backend, settings).Fit(more, harness.Assemble(), {}),
                      "the test tail use 7 tokens, over tail_allowance 1024");
}

TEST_CASE("Fit folds the oldest half of the transcript and condenses an over-cap summary until the prompt fits",
          "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_fit", 1, 8);
    harness.maxEntries = 2;
    harness.maxSummaries = 1;
    std::vector<turn::RetrievalHit> hits;
    REQUIRE(harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), {}));

    // 8 unfolded: fold 1-4. 4 unfolded: fold 5-6. Two summaries over a cap of one: condense. 7 and 8 fit.
    REQUIRE(harness.folded.size() == 2);
    CHECK(harness.folded[0].fromTurn == 1);
    CHECK(harness.folded[0].toTurn == 4);
    CHECK(harness.folded[0].text == "Folded.");
    CHECK(harness.folded[1].fromTurn == 5);
    CHECK(harness.folded[1].toTurn == 6);
    const std::vector<core::Summary> summaries = harness.store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 6);
    CHECK(summaries[0].text == "Condensed.");

    // The fold reads each entry's fold messages after the prompt's system message, with the fold's cap and seed.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 3);
    REQUIRE(calls[0].prompt.messages.size() == 1 + 2 * 4 + 1);
    CHECK(calls[0].prompt.messages[0].content == "SYSTEM");
    CHECK(calls[0].prompt.messages[2].content == "Narration 1.");
    CHECK(calls[0].prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION);
    CHECK(calls[0].spec.maxTokens == 512 / turn::FOLD_SUMMARY_DIVISOR);
    CHECK(calls[0].spec.sampling.seed == core::SamplingSeed(0, 9, core::CallKind::Summarize, 0));
    CHECK(calls[2].prompt.messages.back().content == turn::CONDENSE_INSTRUCTION);
    CHECK(calls[2].spec.maxTokens == 512 / turn::CONDENSE_SUMMARY_DIVISOR);
}

TEST_CASE("Fit returns nothing when a memory call is cancelled, and stores no summary", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_cancel", 1, 4);
    harness.maxEntries = 2;
    std::stop_source stop;
    stop.request_stop();
    std::vector<turn::RetrievalHit> hits;
    CHECK_FALSE(harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), stop.get_token()));
    CHECK(harness.folded.empty());
    CHECK(harness.store.Summaries().empty());
}

TEST_CASE("Settle folds and condenses only while the transcript or the summary is over budget", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_settle", 1, 5);
    harness.maxEntries = 2;
    harness.maxHits = -1; // The tail is not Settle's to fix: between turns it is a placeholder.
    CHECK(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}));

    // 5 unfolded: fold 1-2. 3 unfolded: fold 3. The 2 left fit.
    REQUIRE(harness.folded.size() == 2);
    CHECK(harness.folded[0].fromTurn == 1);
    CHECK(harness.folded[0].toTurn == 2);
    CHECK(harness.folded[1].fromTurn == 3);
    CHECK(harness.folded[1].toTurn == 3);

    // With nothing over budget it makes no call.
    const std::size_t calls = backend.Calls().size();
    CHECK(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}));
    CHECK(backend.Calls().size() == calls);
}

TEST_CASE("FoldAll folds every unfolded entry in one call when it fits, condensing on the way", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_fold_all", 5, 8);
    harness.store.AddSummary({1, 2, "Earlier."});
    harness.store.AddSummary({3, 4, "Later."});
    harness.maxSummaries = 2;
    CHECK(harness.Upkeep(backend, settings).FoldAll(harness.Assemble(), {}));

    // 5-8 fold in one call; three summaries over a cap of two condense into one; nothing is left unfolded.
    REQUIRE(harness.folded.size() == 1);
    CHECK(harness.folded[0].fromTurn == 5);
    CHECK(harness.folded[0].toTurn == 8);
    const std::vector<core::Summary> summaries = harness.store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 8);
    CHECK(backend.Calls().size() == 2);
}

TEST_CASE("A summary still over its cap right after a condense is a DataError", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_over_cap", 1, 0);
    harness.store.AddSummary({1, 1, "Long."});
    harness.maxSummaries = 0; // Even one summary is over the cap.
    CHECK_THROWS_AS(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}), core::DataError);
    CHECK_THROWS_WITH(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}),
                      "summary_cap 512 is too small for even a condensed summary (0 tokens)");
    CHECK(backend.Calls().size() == 2); // One condense per Settle, then the throw instead of a second.
}
```

In `tests/context/RenderTests.cpp`, append:

```cpp
TEST_CASE("TranscriptEntries renders each turn as its L3 messages, its fold messages and its fact", "[context]")
{
    std::vector<core::TurnRecord> turns = NumberedTurns(3);
    turns[0] = TurnWithMechanics();
    turns[0].number = 1;
    turns[0].digest = "The player fell.";

    // proseTurns 1: turns 1 and 2 are older and show their digest in L3; a fold reads every narration.
    const std::vector<context::TranscriptEntry> entries = context::TranscriptEntries(turns, 1);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].turn == 1);
    CHECK(entries[0].messages[1].content == "The player fell.");
    CHECK(entries[0].foldMessages[1].content == "You slip and hit the deck.");
    CHECK(entries[0].fact == "I climb the hull. " + context::RenderMechanicsLine(turns[0]));
    CHECK(entries[1].messages[1].content == "Digest 2.");
    CHECK(entries[1].foldMessages[1].content == "Narration 2.");
    CHECK(entries[1].fact == "Input 2."); // No mechanics: the input alone.
    CHECK(entries[2].messages[1].content == "Narration 3.");

    // The L3 messages are RenderTranscript's, two per turn, in order.
    const std::vector<inference::Message> flat = context::RenderTranscript(turns, 1);
    REQUIRE(flat.size() == 6);
    for (std::size_t i = 0; i < flat.size(); ++i) {
        CHECK(entries[i / 2].messages[i % 2].role == flat[i].role);
        CHECK(entries[i / 2].messages[i % 2].content == flat[i].content);
    }
    CHECK_THROWS_AS(context::TranscriptEntries(turns, 0), core::ContractError);
}
```

In `tests/context/PromptBuilderTests.cpp` (the prompts these tests build do not change; only how the transcript reaches the builder does):

1. Replace

   ```cpp
   #include "TestPaths.h"
   #include "context/PromptBuilder.h"
   ```

   with

   ```cpp
   #include <limits>
   #include <vector>

   #include "TestPaths.h"
   #include "context/PromptBuilder.h"
   #include "context/Transcript.h"
   ```

2. In `Inputs`, replace `    inputs.transcript = {turn};` with

   ```cpp
       inputs.transcript = context::TranscriptEntries({turn}, std::numeric_limits<int>::max());
   ```

3. In the section `transcript one token over its budget`, replace `        inputs.transcript.front().narration = std::string(400, 'x');` with

   ```cpp
           inputs.transcript.front().messages.back().content = std::string(400, 'x');
   ```

4. In the test `Thinking budgets are reserved with the generations they precede`, replace `    inputs.transcript.front().narration = std::string(1200, 'x');` with

   ```cpp
       inputs.transcript.front().messages.back().content = std::string(1200, 'x');
   ```

5. In the test `Digests shrink the transcript layer`, replace

   ```cpp
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
   ```

   with

   ```cpp
       std::vector<core::TurnRecord> turns;
       for (int number = 5; number <= 7; ++number) {
           core::TurnRecord turn;
           turn.number = number;
           turn.input = "I walk on.";
           turn.narration = std::string(400, 'n');
           turn.digest = "The player walked on.";
           turns.push_back(turn);
       }
       inputs.transcript = context::TranscriptEntries(turns, std::numeric_limits<int>::max());
       const context::Assembly full = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
       inputs.transcript = context::TranscriptEntries(turns, 1);
       const context::Assembly compact = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
   ```

   and in the same test replace the comment `// The default shows every narration.` with `// Every narration, as with compact_transcript off.`

In `tests/turn/FoldingTests.cpp` (the fold prompts these tests check do not change; the turns now reach the folding functions as rendered entries):

1. Replace

   ```cpp
   #include <optional>
   #include <stop_token>
   ```

   with

   ```cpp
   #include <limits>
   #include <optional>
   #include <stop_token>
   ```

2. After the function `BlankBackend` (before `} // namespace`), add:

   ```cpp

   // `turns` as the session renders them for L3 with every narration shown; a fold reads the same.
   std::vector<context::TranscriptEntry> Entries(const std::vector<core::TurnRecord>& turns)
   {
       return context::TranscriptEntries(turns, std::numeric_limits<int>::max());
   }
   ```

3. Replace `        system, {Turn(3, "I knock.", "The door creaks."), Turn(4, "I enter.", "It is dark.")}, spec, backend, {});` with `        system, Entries({Turn(3, "I knock.", "The door creaks."), Turn(4, "I enter.", "It is dark.")}), spec, backend, {});`
4. Replace `    CHECK_FALSE(turn::SummarizeTurns({inference::Role::System, "S"}, {Turn(1, "a", "b")}, {}, backend, stop.get_token()));` with `    CHECK_FALSE(turn::SummarizeTurns({inference::Role::System, "S"}, Entries({Turn(1, "a", "b")}), {}, backend, stop.get_token()));`
5. Replace `    CHECK_THROWS_AS(turn::SummarizeTurns(user, {Turn(1, "a", "b")}, {}, backend, {}), core::ContractError);` with `    CHECK_THROWS_AS(turn::SummarizeTurns(user, Entries({Turn(1, "a", "b")}), {}, backend, {}), core::ContractError);`
6. Replace `        turn::SummarizeTurns({inference::Role::System, "S"}, turns, inference::GenerationSpec{}, backend, {});` with `        turn::SummarizeTurns({inference::Role::System, "S"}, Entries(turns), inference::GenerationSpec{}, backend, {});`
7. Replace `        turn::SummarizeTurns({inference::Role::System, "S"}, {Turn(1, std::string(200, 'x'), "b")}, spec, backend, {});` with `        turn::SummarizeTurns({inference::Role::System, "S"}, Entries({Turn(1, std::string(200, 'x'), "b")}), spec, backend, {});`
8. In `The fold and condense prompts and fallbacks are the ones the folding calls use`, replace `    const std::optional<core::Summary> summary = turn::SummarizeTurns(system, turns, spec, backend, {});` with `    const std::optional<core::Summary> summary = turn::SummarizeTurns(system, Entries(turns), spec, backend, {});`, replace `    const inference::Prompt summarizePrompt = turn::SummarizePrompt(system, turns);` with `    const inference::Prompt summarizePrompt = turn::SummarizePrompt(system, Entries(turns));`, and replace `    const core::Summary summaryFallback = turn::SummarizeFallback(turns, spec.maxTokens);` with `    const core::Summary summaryFallback = turn::SummarizeFallback(Entries(turns), spec.maxTokens);`.
9. In `A fold sees the full narration even of turns with a digest`, replace

   ```cpp
       const inference::Prompt prompt =
           turn::SummarizePrompt({inference::Role::System, "story"}, {walked, Turn(2, "I wait.", "Time passes.")});
   ```

   with

   ```cpp
       // proseTurns 1: L3 would show turn 1's digest; the fold still reads its narration.
       const inference::Prompt prompt = turn::SummarizePrompt(
           {inference::Role::System, "story"},
           context::TranscriptEntries({walked, Turn(2, "I wait.", "Time passes.")}, 1));
   ```

In `tests/app/ModelSeatTests.cpp`:

1. Replace

   ```cpp
   #include <optional>
   #include <stop_token>
   ```

   with

   ```cpp
   #include <limits>
   #include <optional>
   #include <stop_token>
   ```

   and replace `#include "app/model/ModelSeat.h"` with

   ```cpp
   #include "app/model/ModelSeat.h"
   #include "context/Transcript.h"
   ```

2. Replace `    CHECK(app::SeatLabelOf(turn::SummarizePrompt(system, {played}), plain) == app::SeatLabel::Summarize);` with

   ```cpp
       CHECK(app::SeatLabelOf(turn::SummarizePrompt(system, context::TranscriptEntries({played}, std::numeric_limits<int>::max())),
                              plain) == app::SeatLabel::Summarize);
   ```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `Cannot open include file: 'turn/MemoryUpkeep.h'` in `tests/turn/MemoryUpkeepTests.cpp` and `'TranscriptEntries': is not a member of 'morpheus::context'`.

- [ ] **Step 3: Render turns as transcript entries**

In `src/context/Transcript.h`, before `// One applied change as the mechanics line and the resolution record show it`, insert:

```cpp
// One turn of a transcript layer (L3), rendered: what the prompt shows, what a fold reads, and the engine's one-line
// account of it for a summary written without a model. Both sessions render their turns into entries; the prompt
// builders and the shared memory upkeep (turn::MemoryUpkeep) work on entries only.
struct TranscriptEntry {
    int turn = 0;                                      // The turn's number, run-wide.
    std::vector<inference::Message> messages = {};     // As L3 shows it: a user and an assistant message.
    std::vector<inference::Message> foldMessages = {}; // As a fold call reads it: the turn in full.
    std::string fact = {};                             // One line; SummarizeFallback writes "Turn N: <fact>".
};

```

and at the end of the file, before `} // namespace morpheus::context`, insert:

```cpp

// Morpheus's own turns as entries: `messages` is the turn's pair from RenderTranscript(turns, proseTurns) (an older
// turn shows its digest), `foldMessages` its pair with every narration, and `fact` its input followed by its
// mechanics line when it has one. Throws ContractError for proseTurns < 1.
std::vector<TranscriptEntry> TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns);
```

In `src/context/Transcript.cpp`, replace

```cpp
#include <cstddef>
#include <format>
#include <utility>
#include <vector>
```

with

```cpp
#include <cstddef>
#include <format>
#include <limits>
#include <utility>
#include <vector>
```

and before `} // namespace morpheus::context` at the end, insert:

```cpp

std::vector<TranscriptEntry> TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns)
{
    // Two messages per turn in both views: as L3 shows it (older turns may show their digest) and in full, for a fold.
    const std::vector<inference::Message> shown = RenderTranscript(turns, proseTurns);
    const std::vector<inference::Message> full = RenderTranscript(turns, std::numeric_limits<int>::max());
    std::vector<TranscriptEntry> entries;
    entries.reserve(turns.size());
    for (std::size_t i = 0; i < turns.size(); ++i) {
        TranscriptEntry entry;
        entry.turn = turns[i].number;
        entry.messages = {shown[2 * i], shown[2 * i + 1]};
        entry.foldMessages = {full[2 * i], full[2 * i + 1]};
        // The engine's own account of the turn: its input and, when it has one, its mechanics line.
        const std::string mechanics = RenderMechanicsLine(turns[i]);
        entry.fact = mechanics.empty() ? turns[i].input : turns[i].input + " " + mechanics;
        entries.push_back(std::move(entry));
    }
    return entries;
}
```

In `src/context/PromptBuilder.h`, replace

```cpp
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Settings.h"
```

with

```cpp
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/Transcript.h"
#include "core/Settings.h"
```

and replace

```cpp
    std::vector<core::TurnRecord> transcript = {}; // L3: unfolded turns, oldest first.
    // L3: the newest turns that show their narration; older ones show their digest (context::RenderTranscript).
    // The default shows every narration.
    int proseTurns = std::numeric_limits<int>::max();
```

with

```cpp
    std::vector<TranscriptEntry> transcript = {};  // L3: unfolded turns, oldest first, rendered (TranscriptEntries).
```

In `src/context/PromptBuilder.cpp`, replace

```cpp
    for (inference::Message& message : RenderTranscript(inputs.transcript, inputs.proseTurns)) {
        tokens.transcript += backend.CountTokens(message.content);
        prompt.messages.push_back(std::move(message));
    }
```

with

```cpp
    for (const TranscriptEntry& entry : inputs.transcript) {
        for (const inference::Message& message : entry.messages) {
            tokens.transcript += backend.CountTokens(message.content);
            prompt.messages.push_back(message);
        }
    }
```

- [ ] **Step 4: Fold entries**

Replace the whole of `src/turn/Folding.h` with:

```cpp
#pragma once

#include <optional>
#include <stop_token>
#include <vector>

#include "context/Transcript.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Rough bytes per token; the fallback only needs to stay near the summary's token cap.
constexpr int FALLBACK_BYTES_PER_TOKEN = 4;

// The fold call's prompt: `systemMessage` (the prompt's system message, L0 up to L2), each entry's foldMessages, then
// SUMMARIZE_INSTRUCTION. stableCount ends after the entries. The memory upkeep measures it before a fold.
inference::Prompt SummarizePrompt(const inference::Message& systemMessage,
                                  const std::vector<context::TranscriptEntry>& entries);

// The engine's summary of `entries`, written without a model: one "Turn N: <fact>" line per entry, cut to
// maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers the entries' range (the first's turn to the last's). Throws
// ContractError for no entries.
core::Summary SummarizeFallback(const std::vector<context::TranscriptEntry>& entries, int maxTokens);

// Summarizes `entries` (the oldest unfolded turns, in order) with one call on SummarizePrompt. That prompt starts like
// the turn's prompt, so the KV cache serves most of it. The summary covers the entries' range; its text is trimmed.
// When the model writes nothing, the summary is SummarizeFallback(entries, spec.maxTokens).
// Returns nullopt when `stop` cancels the call. Throws ContractError for no entries or a non-system message, and
// InferenceError from the backend (among others when the prompt does not fit the context: the caller measures it).
std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<context::TranscriptEntry>& entries,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop);

// The condense call's prompt: `systemMessage` (whose L2 holds the summaries), then CONDENSE_INSTRUCTION.
inference::Prompt CondensePrompt(const inference::Message& systemMessage);

// The engine's condensed summary, written without a model: one "Turns A-B: <text>" line per summary, cut to
// maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers every summary's range. Throws ContractError for none.
core::Summary CondenseFallback(const std::vector<core::Summary>& summaries, int maxTokens);

// Condenses every summary into one covering their whole range, with one call on CondensePrompt. When the model
// writes nothing, the summary is CondenseFallback(summaries, spec.maxTokens).
// Returns nullopt when cancelled. Throws ContractError for no summaries or a non-system message, and
// InferenceError from the backend.
std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop);

} // namespace morpheus::turn
```

Replace the whole of `src/turn/Folding.cpp` with:

```cpp
#include "turn/Folding.h"

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

inference::Prompt SummarizePrompt(const inference::Message& systemMessage,
                                  const std::vector<context::TranscriptEntry>& entries)
{
    // The system message, then each turn in full (stable, so cached), then the instruction. Summaries are written
    // from the full narration, digests or not.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (const context::TranscriptEntry& entry : entries) {
        for (const inference::Message& message : entry.foldMessages) {
            prompt.messages.push_back(message);
        }
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, std::string(SUMMARIZE_INSTRUCTION)});
    return prompt;
}

core::Summary SummarizeFallback(const std::vector<context::TranscriptEntry>& entries, int maxTokens)
{
    MORPHEUS_REQUIRE(!entries.empty() && maxTokens > 0, "SummarizeFallback: needs at least one entry and a token cap");
    // Each turn's fact: what the engine knows without a model.
    std::string text;
    for (const context::TranscriptEntry& entry : entries) {
        text += std::format("{}Turn {}: {}", text.empty() ? "" : "\n", entry.turn, entry.fact);
    }
    // Cut to about the summary's token cap.
    text = core::CutUtf8(text, static_cast<std::size_t>(maxTokens) * FALLBACK_BYTES_PER_TOKEN);
    return core::Summary{entries.front().turn, entries.back().turn, std::move(text)};
}

std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<context::TranscriptEntry>& entries,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !entries.empty(),
                     "SummarizeTurns: needs the system message and at least one entry");
    const inference::Completion completion = backend.Complete(SummarizePrompt(systemMessage, entries), spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }

    // The model's summary, trimmed. When it wrote nothing, the engine writes a factual one so folding still
    // makes progress.
    std::string text = core::Trim(completion.text);
    if (text.empty()) {
        return SummarizeFallback(entries, spec.maxTokens);
    }
    return core::Summary{entries.front().turn, entries.back().turn, std::move(text)};
}

inference::Prompt CondensePrompt(const inference::Message& systemMessage)
{
    // The system message already holds every summary (L2), so only the instruction follows it.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    prompt.stableCount = 1;
    prompt.messages.push_back({inference::Role::User, std::string(CONDENSE_INSTRUCTION)});
    return prompt;
}

core::Summary CondenseFallback(const std::vector<core::Summary>& summaries, int maxTokens)
{
    MORPHEUS_REQUIRE(!summaries.empty() && maxTokens > 0, "CondenseFallback: needs at least one summary and a token cap");
    // Every summary's text under its range, cut to about the condensed summary's token cap.
    std::string text;
    for (const core::Summary& summary : summaries) {
        text += std::format("{}Turns {}-{}: {}", text.empty() ? "" : "\n", summary.fromTurn, summary.toTurn,
                            summary.text);
    }
    text = core::CutUtf8(text, static_cast<std::size_t>(maxTokens) * FALLBACK_BYTES_PER_TOKEN);
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, std::move(text)};
}

std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !summaries.empty(),
                     "CondenseSummaries: needs the system message and at least one summary");
    const inference::Completion completion = backend.Complete(CondensePrompt(systemMessage), spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }

    // The model's summary, trimmed. When it wrote nothing, the engine writes one from the summaries it was given.
    std::string text = core::Trim(completion.text);
    if (text.empty()) {
        return CondenseFallback(summaries, spec.maxTokens);
    }
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, std::move(text)};
}

} // namespace morpheus::turn
```

- [ ] **Step 5: Write the memory upkeep**

Create `src/turn/MemoryUpkeep.h`:

```cpp
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// A turn that cannot run as asked: nothing to undo or regenerate, an empty input, or an input too long for the
// tail allowance. Nothing changed; the message is for the player (in a hosted run, for the host).
class TurnError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr int FOLD_SUMMARY_DIVISOR = 4;     // A fold summary gets summary_cap / 4 tokens: several fit before a condense.
constexpr int CONDENSE_SUMMARY_DIVISOR = 2; // A condensed summary gets summary_cap / 2 tokens, so it fits its cap.

// One assembled prompt and the transcript entries its L3 shows, oldest first: what the upkeep measures and folds.
struct FitPass {
    context::Assembly assembly = {};
    std::vector<context::TranscriptEntry> transcript = {};
};

// The memory upkeep both sessions share (hosted-world spec, "Shared memory upkeep"): it keeps a prompt within its
// budgets by shedding retrieval hits, condensing the summaries and folding the oldest transcript entries into a new
// summary. It works on summaries and rendered entries only, so turn::Session passes its turns and
// turn::HostedSession its conversation's turns. Built for one turn's memory calls (their sampling seeds name the
// turn) and used at once: it holds the session's store, backend and settings by pointer.
class MemoryUpkeep {
public:
    // Assembles the prompt again for `hits`, from the store as it is now: the upkeep calls it after every fold,
    // condense and shed hit.
    using Assemble = std::function<FitPass(const std::vector<RetrievalHit>& hits)>;
    // Stores a fold's summary and makes the turns it covers searchable history: each session records what it folded
    // its own way.
    using StoreFold = std::function<void(const core::Summary& summary)>;

private:
    store::RunStore* _pStore = nullptr;         // Not owned; the session's.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; the session's.
    const core::Settings* _pSettings = nullptr; // Not owned; the session's.
    std::string _tailName = {};                 // What the tail holds, for the TurnError when it is too long.
    int _turn = 0;                              // The turn these memory calls are for (their sampling seeds).
    int _attempt = 0;
    StoreFold _storeFold = {};

public:
    // `tailName` completes "<tailName> use N tokens, over tail_allowance M". Throws ContractError for an empty
    // storeFold.
    MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings,
                 std::string tailName, int turn, int attempt, StoreFold storeFold);

    // Assembles a prompt that fits, from the `hits` it may shed (from the end) and the store's summaries and
    // transcript. Every pass sheds a hit while the tail is over its allowance, condenses an over-cap summary, folds the
    // oldest half (at least one) of an over-budget or overflowing transcript, or sheds a hit as a last resort.
    // Returns the prompt, or nullopt when a memory call was cancelled (what it folded before stays). Throws TurnError
    // when the tail is over its allowance with no hit left or nothing fits at all, and DataError when a summary is
    // still over summary_cap right after a condense.
    std::optional<context::Assembly> Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble,
                                         std::stop_token stop);

    // Between turns (Session::Maintain): condenses while the summary is over its cap and folds the oldest half while
    // the transcript is over its budget, with no hits. False when cancelled. Throws DataError like Fit.
    bool Settle(const Assemble& assemble, std::stop_token stop);

    // An ended conversation (HostedSession::Maintain): folds until no entry is left, every remaining entry in one
    // call when it fits, condensing whenever the summary is over its cap. False when cancelled. Throws DataError
    // like Fit.
    bool FoldAll(const Assemble& assemble, std::stop_token stop);

private:
    // Folds the oldest `wanted` entries of `transcript` into a new summary, or as many of them as fit one call in the
    // context. When not even one fits, the engine writes that entry's summary without a model call
    // (SummarizeFallback). Every call folds at least one entry and hands the summary to StoreFold. False when
    // cancelled.
    bool FoldOldest(const inference::Message& systemMessage, const std::vector<context::TranscriptEntry>& transcript,
                    std::size_t wanted, std::stop_token stop);

    // Replaces every summary with one condensed summary. When the condense call does not fit the context, the engine
    // writes it without a model call (CondenseFallback). False when cancelled.
    bool Condense(const inference::Message& systemMessage, std::stop_token stop);
};

} // namespace morpheus::turn
```

Create `src/turn/MemoryUpkeep.cpp`:

```cpp
#include "turn/MemoryUpkeep.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "turn/Folding.h"
#include "turn/Session.h"

namespace morpheus::turn {

namespace {

// Thrown when a summary is still over summary_cap right after a condense: condensing again would only repeat it.
core::DataError CondensedOverCap(int summaryCap, int summaryTokens)
{
    return core::DataError(std::format("summary_cap {} is too small for even a condensed summary ({} tokens)",
                                       summaryCap, summaryTokens));
}

} // namespace

MemoryUpkeep::MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings,
                           std::string tailName, int turn, int attempt, StoreFold storeFold)
    : _pStore(&store)
    , _pBackend(&backend)
    , _pSettings(&settings)
    , _tailName(std::move(tailName))
    , _turn(turn)
    , _attempt(attempt)
    , _storeFold(std::move(storeFold))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_storeFold), "MemoryUpkeep: a fold needs somewhere to go");
}

std::optional<context::Assembly> MemoryUpkeep::Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble,
                                                   std::stop_token stop)
{
    // Every pass sheds a hit, condenses the summary, or folds turns. Hits and the transcript only shrink, and a
    // summary still over its cap right after a condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        FitPass pass = assemble(hits);
        const context::Assembly& assembly = pass.assembly;
        if (assembly.Fits()) {
            return std::move(pass.assembly);
        }
        if (assembly.tailOverBudget) {
            if (hits.empty()) {
                throw TurnError(std::format("{} use {} tokens, over tail_allowance {}", _tailName, assembly.tokens.tail,
                                            _pSettings->budget.tailAllowance));
            }
            hits.pop_back();
            continue;
        }
        if (assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, assembly.tokens.summary);
            }
            if (!Condense(assembly.prompt.messages.front(), stop)) {
                return std::nullopt;
            }
            condensed = true;
            continue;
        }
        // The transcript is over its budget, or the whole prompt overflows. contextOverflow is the authoritative fit
        // check: transcriptBudget leaves out a few template tokens that it counts.
        if (!pass.transcript.empty()) {
            const std::size_t half = std::max<std::size_t>(1, pass.transcript.size() / 2);
            if (!FoldOldest(assembly.prompt.messages.front(), pass.transcript, half, stop)) {
                return std::nullopt;
            }
            condensed = false;
            continue;
        }
        if (!hits.empty()) {
            hits.pop_back();
            continue;
        }
        throw TurnError("the prompt does not fit context_size even without transcript or retrieval");
    }
}

bool MemoryUpkeep::Settle(const Assemble& assemble, std::stop_token stop)
{
    // Every pass condenses or folds. A fold shrinks the transcript, and a summary still over its cap right after a
    // condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        const FitPass pass = assemble({});
        if (pass.assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, pass.assembly.tokens.summary);
            }
            if (!Condense(pass.assembly.prompt.messages.front(), stop)) {
                return false;
            }
            condensed = true;
            continue;
        }
        if (pass.assembly.transcriptOverBudget && !pass.transcript.empty()) {
            const std::size_t half = std::max<std::size_t>(1, pass.transcript.size() / 2);
            if (!FoldOldest(pass.assembly.prompt.messages.front(), pass.transcript, half, stop)) {
                return false;
            }
            condensed = false;
            continue;
        }
        return true;
    }
}

bool MemoryUpkeep::FoldAll(const Assemble& assemble, std::stop_token stop)
{
    // Every pass condenses or folds at least one entry, so the transcript empties and the loop ends; a summary still
    // over its cap right after a condense throws.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        const FitPass pass = assemble({});
        if (pass.assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, pass.assembly.tokens.summary);
            }
            if (!Condense(pass.assembly.prompt.messages.front(), stop)) {
                return false;
            }
            condensed = true;
            continue;
        }
        if (!pass.transcript.empty()) {
            // All of it at once; FoldOldest folds the longest prefix that fits when that is too much for one call.
            if (!FoldOldest(pass.assembly.prompt.messages.front(), pass.transcript, pass.transcript.size(), stop)) {
                return false;
            }
            condensed = false;
            continue;
        }
        return true;
    }
}

bool MemoryUpkeep::FoldOldest(const inference::Message& systemMessage,
                              const std::vector<context::TranscriptEntry>& transcript, std::size_t wanted,
                              std::stop_token stop)
{
    MORPHEUS_REQUIRE(!transcript.empty() && wanted >= 1 && wanted <= transcript.size(),
                     "FoldOldest: needs 1 to transcript.size() entries to fold");
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _pSettings->budget.summaryCap / FOLD_SUMMARY_DIVISOR);
    spec.sampling = SamplingFrom(_pSettings->resolveSampling, core::SamplingSeed(_pStore->Meta().rngSeed, _turn,
                                                                                core::CallKind::Summarize, _attempt));

    // The oldest `count` entries of the transcript.
    auto oldest = [&transcript](std::size_t count) {
        return std::vector<context::TranscriptEntry>(transcript.begin(),
                                                     transcript.begin() + static_cast<std::ptrdiff_t>(count));
    };
    // Whether a fold call over the oldest `count` entries fits the context: its prompt plus the summary's max_tokens,
    // measured exactly as the backend measures a call. The transcript can be far larger than the context (a run
    // loaded with a smaller context_size, or turns put back by undoing past a condensed summary).
    auto fits = [&](std::size_t count) {
        const int promptTokens = _pBackend->CountPromptTokens(SummarizePrompt(systemMessage, oldest(count)));
        return spec.maxTokens <= _pBackend->ContextSize() - promptTokens;
    };

    // Fold the wanted entries, or the longest prefix of them that fits. More entries never cost fewer tokens, so a
    // binary search between "fits" and "does not fit" finds that prefix with few token counts.
    std::size_t count = wanted;
    if (!fits(wanted)) {
        std::size_t fitting = 0;      // Longest prefix known to fit; 0 means none is known to.
        std::size_t tooLong = wanted; // Shortest prefix known not to fit.
        while (tooLong - fitting > 1) {
            const std::size_t middle = fitting + (tooLong - fitting) / 2;
            if (fits(middle)) {
                fitting = middle;
            } else {
                tooLong = middle;
            }
        }
        count = fitting;
    }

    std::optional<core::Summary> summary;
    if (count == 0) {
        // Not even the oldest entry fits a call: the engine writes its summary, so folding always makes progress.
        summary = SummarizeFallback(oldest(1), spec.maxTokens);
    } else {
        summary = SummarizeTurns(systemMessage, oldest(count), spec, *_pBackend, stop);
        if (!summary) {
            return false;
        }
    }
    // The session stores it and makes the folded turns searchable history.
    _storeFold(*summary);
    return true;
}

bool MemoryUpkeep::Condense(const inference::Message& systemMessage, std::stop_token stop)
{
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _pSettings->budget.summaryCap / CONDENSE_SUMMARY_DIVISOR);
    spec.sampling = SamplingFrom(_pSettings->resolveSampling, core::SamplingSeed(_pStore->Meta().rngSeed, _turn,
                                                                                core::CallKind::Summarize, _attempt));
    const std::vector<core::Summary> summaries = _pStore->Summaries();

    // The condense prompt holds every summary in its system message. Summaries written under a larger summary_cap
    // can make it larger than the context: then the engine condenses them itself, so the loop still makes progress.
    const int promptTokens = _pBackend->CountPromptTokens(CondensePrompt(systemMessage));
    std::optional<core::Summary> merged;
    if (spec.maxTokens <= _pBackend->ContextSize() - promptTokens) {
        merged = CondenseSummaries(systemMessage, summaries, spec, *_pBackend, stop);
        if (!merged) {
            return false;
        }
    } else {
        merged = CondenseFallback(summaries, spec.maxTokens);
    }
    _pStore->ReplaceSummaries(*merged);
    return true;
}

} // namespace morpheus::turn
```

In `src/turn/CMakeLists.txt`, replace

```cmake
    Folding.cpp
    Replay.cpp
```

with

```cmake
    Folding.cpp
    MemoryUpkeep.cpp
    Replay.cpp
```

- [ ] **Step 6: Make Session call the upkeep**

Replace the whole of `src/turn/Session.h` with:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/MemoryUpkeep.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

constexpr std::size_t RETRIEVAL_MAX_HITS = 6; // Retrieval hits in the tail (T2), before retrieval_cap applies.
constexpr std::size_t LOOKUP_MAX_HITS = 2;    // Hits each assisted-mode lookup adds.
constexpr int LOOKUP_MAX_TOKENS = 192;        // Generation cap for the lookup call.
// Added to a turn's attempt for the seed of its second review, so that seed never equals a regenerated turn's first
// review seed (attempt + 1). PlayTurn requires attempt < REVIEW_RETRY_OFFSET, which keeps the sum within
// SamplingSeed's limit of 2^24.
constexpr int REVIEW_RETRY_OFFSET = 1 << 23;

// The sampling for one call from its settings.json values and the call's seed (core::SamplingSeed).
inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);

struct TurnCallbacks {
    // After apply, before narrate: the record has its checks and applied and rejected changes, no narration yet.
    std::function<void(const core::TurnRecord&)> onResolved = {};
    // Every narration's pieces as they are written, the renarrate's included.
    inference::TokenCallback onNarrationToken = {};
    // The review found that the narration contradicts the outcome (`problems` are its reasons): the narration starts
    // over, and the new one streams through onNarrationToken. At most once per turn.
    std::function<void(const std::vector<std::string>& problems)> onNarrationRestarted = {};
};

// One open run: its world, settings, database and memory index, and the turn loop (architecture spec, "Turn flow").
// Every backend call happens inside these methods, on the caller's thread (the engine worker). Not thread-safe.
class Session {
private:
    core::World _world = {}; // Set by the constructor.
    core::Settings _settings = {};
    core::MemoryMode _mode = core::MemoryMode::Engine;
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index;     // Set by the constructor.
    context::Assembly _lastAssembly = {};
    bool _worldChanged = false;

public:
    // Starts a new run in `file` from the world's initial state, with `seed` for dice and sampling, and indexes the
    // lore. Throws StoreError if the file exists, and DataError if lore cannot be embedded (then no file is left).
    static Session Start(core::World world, core::Settings settings, const std::filesystem::path& file,
                         std::uint64_t seed, inference::LlmBackend& backend);

    // Opens a run of `world`. Throws DataError if the run belongs to another world or its latest state no longer
    // validates against the world's schema. Edited world files set WorldChanged(); edited lore is re-embedded.
    static Session Load(core::World world, core::Settings settings, const std::filesystem::path& file,
                        inference::LlmBackend& backend);

    // Plays one turn: retrieve, (assisted mode: look up), resolve, apply, narrate, (narration_check or
    // compact_transcript: review, and at most one renarrate with its own review), commit. Control characters other
    // than newline and tab are stripped from `input` and it is trimmed. Returns the committed record, or nullopt
    // when `stop` cancelled the turn. Throws TurnError for an empty input or one that does not fit, DataError when
    // summary_cap is too small for even a condensed summary, and InferenceError from the backend. A cancelled or
    // failed turn commits no turn, but memory upkeep its fit loop finished before the stop (summaries and their
    // history embeddings) is kept: it only covers committed turns.
    std::optional<core::TurnRecord> RunTurn(std::string_view input, const TurnCallbacks& callbacks,
                                            std::stop_token stop);

    // Removes the latest turn, with its notes and any summary covering it, from the run and from retrieval.
    // Throws TurnError when there is none.
    void Undo();

    // Plays the latest turn's input again as a new attempt: same dice, newly sampled text. If it is cancelled or
    // fails, the previous turn is put back with the summaries that covered it. Throws TurnError when there is no
    // turn.
    std::optional<core::TurnRecord> Regenerate(const TurnCallbacks& callbacks, std::stop_token stop);

    // Between-turn upkeep: folds or condenses while the transcript or the summary is over budget, then indexes
    // folded turns and notes. Returns false if `stop` cancelled it (it can simply run again later). Throws DataError
    // when summary_cap is too small for even a condensed summary.
    bool Maintain(std::stop_token stop);

    const store::RunMeta& Meta() const;
    bool WorldChanged() const;
    nlohmann::json State();                  // After the latest turn.
    std::vector<core::TurnRecord> History(); // Every committed turn, oldest first.
    std::vector<core::Summary> Summaries();
    std::vector<core::Note> Notes(); // Every committed note, by turn, then the order written.
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the context inspector.

private:
    Session(core::World world, core::Settings settings, store::RunStore store, MemoryIndex index,
            inference::LlmBackend& backend);

    // RunTurn and Regenerate: plays turn LatestTurn()+1 for a cleaned input.
    std::optional<core::TurnRecord> PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                             std::stop_token stop);

    // One pass of the memory upkeep: the prompt for `hits` and the given tail parts, with the summaries and the
    // unfolded transcript read from the store as it is now, and the transcript entries it shows.
    FitPass Pass(const nlohmann::json& state, const std::vector<RetrievalHit>& hits, const std::string& input,
                 std::string_view instruction);

    // The shared memory upkeep for turn `turn`'s memory calls: a fold's summary joins the store, and the turns it
    // covers become searchable history.
    MemoryUpkeep Upkeep(int turn, int attempt);

    // The last turn covered by a summary, 0 when none.
    int FoldedThrough();
};

} // namespace morpheus::turn
```

In `src/turn/Session.cpp`:

1. Replace

   ```cpp
   #include "core/Dice.h"
   #include "core/Errors.h"
   #include "core/Text.h"
   #include "turn/EngineText.h"
   #include "turn/Folding.h"
   #include "turn/Resolve.h"
   ```

   with

   ```cpp
   #include "context/Transcript.h"
   #include "core/Dice.h"
   #include "core/Errors.h"
   #include "core/Text.h"
   #include "turn/EngineText.h"
   #include "turn/Resolve.h"
   ```

2. Delete the function `CondensedOverCap` from the anonymous namespace (its comment `// Thrown when a summary is still over summary_cap right after a condense: condensing again would only repeat it.`, the function and the blank line after it). It lives in `MemoryUpkeep.cpp` now.

3. Replace the whole body of `bool Session::Maintain(std::stop_token stop)` (from its opening brace to its closing brace) with:

   ```cpp
   {
       const int latest = _store.LatestTurn();
       const nlohmann::json state = _store.StateAt(latest);
       // The next input is unknown: a placeholder tail measures the stable layers. The turn checks the real tail.
       auto assemble = [&](const std::vector<RetrievalHit>&) {
           return Pass(state, {}, std::string(MAINTENANCE_INPUT), RESOLVE_INSTRUCTION);
       };
       if (!Upkeep(latest, 0).Settle(assemble, stop)) {
           return false;
       }
       _index.SyncHistory(_store, FoldedThrough(), *_pBackend);
       return true;
   }
   ```

4. In `PlayTurn`, replace

   ```cpp
           std::optional<context::Assembly> lookupAssembly =
               AssembleFitting(state, hits, input, LOOKUP_INSTRUCTION, turn, attempt, stop);
   ```

   with

   ```cpp
           std::optional<context::Assembly> lookupAssembly = Upkeep(turn, attempt).Fit(
               hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(state, kept, input, LOOKUP_INSTRUCTION); },
               stop);
   ```

   and replace

   ```cpp
       std::optional<context::Assembly> assembly =
           AssembleFitting(state, hits, input, RESOLVE_INSTRUCTION, turn, attempt, stop);
   ```

   with

   ```cpp
       std::optional<context::Assembly> assembly = Upkeep(turn, attempt).Fit(
           hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(state, kept, input, RESOLVE_INSTRUCTION); },
           stop);
   ```

5. Replace everything from `context::ContextInputs Session::Inputs(const nlohmann::json& state, const std::vector<RetrievalHit>& hits,` up to (not including) `int Session::FoldedThrough()` (the definitions of `Inputs`, `AssembleFitting`, `FoldOldest` and `Condense`) with:

   ```cpp
   FitPass Session::Pass(const nlohmann::json& state, const std::vector<RetrievalHit>& hits, const std::string& input,
                         std::string_view instruction)
   {
       context::ContextInputs inputs;
       inputs.engineRules = std::string(ENGINE_RULES);
       if (_mode == core::MemoryMode::Assisted) {
           inputs.engineRules += "\n\n" + std::string(ASSISTED_RULES);
       }
       inputs.summaries = _store.Summaries();
       // L3: the unfolded turns. With the compact transcript on, older turns show their digest; otherwise every turn
       // shows its narration. A fold always reads the full narration (TranscriptEntry::foldMessages).
       const int latest = _store.LatestTurn();
       const int firstUnfolded = FoldedThrough() + 1;
       if (firstUnfolded <= latest) {
           const int proseTurns = _settings.compactTranscript ? _settings.proseTurns : std::numeric_limits<int>::max();
           inputs.transcript = context::TranscriptEntries(_store.Turns(firstUnfolded, latest), proseTurns);
       }
       inputs.state = state;
       for (const RetrievalHit& hit : hits) {
           inputs.retrieved.push_back(hit.text);
       }
       inputs.playerInput = input;
       inputs.instruction = std::string(instruction);

       FitPass pass;
       pass.assembly = context::AssembleResolvePrompt(_world, inputs, _settings.budget, *_pBackend);
       pass.transcript = std::move(inputs.transcript);
       return pass;
   }

   MemoryUpkeep Session::Upkeep(int turn, int attempt)
   {
       // A fold's summary joins the store, and the turns it covers leave the transcript and become searchable history.
       return MemoryUpkeep(_store, *_pBackend, _settings, "the input and the state", turn, attempt,
                           [this](const core::Summary& summary) {
                               _store.AddSummary(summary);
                               _index.SyncHistory(_store, summary.toTurn, *_pBackend);
                           });
   }

   ```

Check: `grep -n "AssembleFitting\|FoldOldest\|Inputs(\|CondensedOverCap" src/turn/Session.h src/turn/Session.cpp` prints nothing.

- [ ] **Step 7: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[upkeep],[folding],[context],[session],[retrieval],[replay]"`
Expected: builds with no warnings; `All tests passed`. No `Session` test was changed.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 446` (427 + 7 + 5 + 7), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 8: Build the dev targets**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings (the tools and the app use `Session`'s public API and `context::ContextInputs` without a transcript, both unchanged for them).

- [ ] **Step 9: Byte check and commit**

```bash
FILES="src/context/Transcript.h src/context/Transcript.cpp src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/Folding.h src/turn/Folding.cpp src/turn/MemoryUpkeep.h src/turn/MemoryUpkeep.cpp src/turn/Session.h src/turn/Session.cpp src/turn/CMakeLists.txt tests/turn/MemoryUpkeepTests.cpp tests/context/RenderTests.cpp tests/context/PromptBuilderTests.cpp tests/turn/FoldingTests.cpp tests/app/ModelSeatTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Move Session's fit loop, fold and condense into turn::MemoryUpkeep, working on rendered transcript entries" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

