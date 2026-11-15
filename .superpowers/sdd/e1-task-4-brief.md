### Task 4: The speaker's memories in the tail, the speaker's L3, and the speaker's fold

**Files:**
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp` (hosted inputs and rendering)
- Modify: `src/turn/EngineText.h` (two hosted texts)
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Test: `tests/context/HostedPromptTests.cpp`, `tests/hosted/HostedMemoryTests.cpp` (new), and the hosted tests whose expectations move with L2

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces:
  - `struct context::MemoryLine { std::string toldBy = {}; std::string text = {}; };` (`toldBy` is the teller's name, empty for the speaker's own).
  - `context::HostedInputs` loses `summaries` and gains `std::string speakerName = {};` and `std::vector<MemoryLine> memories = {};`.
  - `std::string context::RenderMemories(std::string_view speakerName, const std::vector<MemoryLine>& memories)`: `"# What <name> remembers\n"` then `"- (told by <teller>) <text>\n"` or `"- <text>\n"` per memory; `""` for none.
  - `turn::HOSTED_SUMMARIZE_INSTRUCTION`, `turn::HOSTED_CONDENSE_INSTRUCTION` (each with `{actor}`).
  - `HostedSession` private: `std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, int afterTurn, int throughTurn, std::string_view witnessId)` (turns with `afterTurn < number <= throughTurn`, only those `witnessId` witnessed when it is not empty), `FitPass Pass(std::string_view conversationId, std::string_view actorId, std::vector<context::TranscriptEntry> transcript, const std::vector<RetrievalHit>& hits, context::HostedInputs inputs)`, `MemoryUpkeep Upkeep(const std::string& conversationId, const std::string& actorId, int turn, bool foldsConversation)`.
  - `HostedSession::Open` throws `core::DataError` naming `summary_cap` and `tail_allowance` when `summary_cap >= tail_allowance`.

- [ ] **Step 1: Write the failing tests**

In `tests/context/HostedPromptTests.cpp` (read its helpers first: it builds `HostedInputs` and calls `AssembleHostedPrompt` with a fake backend), add:

```cpp
TEST_CASE("A hosted prompt shows the speaker's memories in the tail, with their tellers, and no L2", "[context][hosted]")
{
    context::HostedInputs inputs = ExampleHostedInputs(); // The file's helper; use its name.
    inputs.speakerName = "Saadia";
    inputs.memories = {{"", "Saadia arrived in Whiterun in spring."}, {"Hulda", "The guard captain takes bribes."}};
    test::FakeBackend backend(16384);
    const context::Assembly assembly = context::AssembleHostedPrompt(inputs, ExampleBudget(), backend);
    const std::string& system = assembly.prompt.messages.front().content;
    const std::string& tail = assembly.prompt.messages.back().content;
    CHECK(system.find("arrived in Whiterun") == std::string::npos);
    CHECK(tail.starts_with("# What Saadia remembers\n- Saadia arrived in Whiterun in spring.\n- (told by Hulda) The guard "
                           "captain takes bribes.\n"));
    CHECK(assembly.tokens.summary ==
          backend.CountTokens(context::RenderMemories("Saadia", inputs.memories)));
    // No memories: no heading.
    inputs.memories.clear();
    const context::Assembly empty = context::AssembleHostedPrompt(inputs, ExampleBudget(), backend);
    CHECK(empty.prompt.messages.back().content.find("remembers") == std::string::npos);
}
```

(Use the file's helpers for a valid `HostedInputs` and budget; adapt the names `ExampleHostedInputs` and `ExampleBudget` to the ones it has. Update its existing tests that expected the summaries in the system message: they now expect them, as memories, in the tail.)

Create `tests/hosted/HostedMemoryTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

} // namespace

TEST_CASE("Two NPCs taking turns in one conversation share the prompt's prefix up to the tail", "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"Welcome.", "Hello there.", "Good day."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_prefix"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "And you?"), {}, {}));
    const inference::Prompt hulda = session.LastAssembly().prompt;
    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "And you, Saadia?"), {}, {}));
    const inference::Prompt saadia = session.LastAssembly().prompt;

    // Saadia's prompt starts with everything Hulda's stable prefix held.
    REQUIRE(saadia.messages.size() >= hulda.stableCount);
    for (std::size_t k = 0; k < hulda.stableCount; ++k) {
        INFO("message " << k);
        CHECK(saadia.messages[k].role == hulda.messages[k].role);
        CHECK(saadia.messages[k].content == hulda.messages[k].content);
    }
}

TEST_CASE("A fold during a turn writes the speaker's memory, in the speaker's words", "[hosted][memory]")
{
    // A transcript budget small enough that the third turn folds the oldest turns.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {std::string(3000, 'a'), std::string(3000, 'b'), "Short."};
    test::ScriptedBackend backend(4096, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = 4096;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_fold"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "One."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Two."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Three."), {}, {}));

    // Hulda, the speaker, remembers the folded turns; the fold call named her.
    CHECK(session.Conversation(id)->foldedThrough >= 1);
    const std::vector<core::Summary> hulda = session.Memories(test::HULDA);
    REQUIRE_FALSE(hulda.empty());
    CHECK(hulda[0].conversationId == id);
    bool named = false;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        named = named || call.prompt.messages.back().content.find("as Hulda remembers") != std::string::npos;
    }
    CHECK(named);
}

TEST_CASE("A hosted run's summary_cap must be below its tail_allowance", "[hosted][memory]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.budget.summaryCap = settings.budget.tailAllowance;
    CHECK_THROWS_AS(turn::HostedSession::Open(test::HostedRunFile("hosted_memory_cap"), test::FixtureHostData(), settings,
                                              42, backend),
                    core::DataError);
    CHECK_THROWS_WITH(turn::HostedSession::Open(test::HostedRunFile("hosted_memory_cap2"), test::FixtureHostData(),
                                                settings, 42, backend),
                      ContainsSubstring("summary_cap") && ContainsSubstring("tail_allowance"));
}
```

(The fold test's sizes: with a 4096 context and HostedSettings' budgets, two 3000-byte lines (750 tokens each) put L3 over its budget on the third turn. If the fake counts differently, tune the line lengths so `foldedThrough` moves, and say so in the report. If HostedSettings' budgets do not leave room for L3 in 4096 tokens, use 8192 and 6000-byte lines.)

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`speakerName` is not a member of `HostedInputs`).

- [ ] **Step 3: The hosted prompt**

In `src/context/PromptBuilder.h`, before `HostedInputs`:

```cpp
// One memory of a hosted speaker, as its prompt shows it: its text, and the teller's name for one told by another NPC.
struct MemoryLine {
    std::string toldBy = {}; // Empty for the speaker's own memory.
    std::string text = {};
};
```

In `HostedInputs`, remove `summaries` and add, before `retrieved`:

```cpp
    std::string speakerName = {};          // The speaker, for the memories' heading.
    std::vector<MemoryLine> memories = {}; // Tail: the speaker's shown summaries, oldest first.
```

Declare after `AssembleHostedPrompt`:

```cpp
// The speaker's memories as a hosted tail shows them: "# What <name> remembers", then one line per memory ("- (told by
// <teller>) <text>" for one told by another NPC). Empty for none. The condense call of a hosted NPC shows the same.
std::string RenderMemories(std::string_view speakerName, const std::vector<MemoryLine>& memories);
```

In `src/context/PromptBuilder.cpp`:

```cpp
std::string RenderMemories(std::string_view speakerName, const std::vector<MemoryLine>& memories)
{
    if (memories.empty()) {
        return {};
    }
    std::string text = std::format("# What {} remembers\n", speakerName);
    for (const MemoryLine& memory : memories) {
        text += memory.toldBy.empty() ? std::format("- {}\n", memory.text)
                                      : std::format("- (told by {}) {}\n", memory.toldBy, memory.text);
    }
    return text;
}
```

In `AssembleHostedPrompt`: drop the L2 summary text from the system message (the system text is `inputs.systemText + "\n\n" + worldText`), and start the tail with the memories:

```cpp
    // The tail, rebuilt every turn, so it goes last: the speaker's memories (they change with the speaker, so they sit
    // after the conversation the speakers share), retrieved snippets, the observation, the new events, the player's
    // line and the instruction.
    const std::string memories = RenderMemories(inputs.speakerName, inputs.memories);
    std::string tail = memories.empty() ? std::string() : memories + "\n";
```

and set `tokens.summary = backend.CountTokens(memories);` where it counted the summary text, then call `LayOut(..., budget, SummaryPlace::Tail, backend)`.

- [ ] **Step 4: The hosted texts**

In `src/turn/EngineText.h`, with the other hosted texts:

```cpp
// A hosted NPC's fold call (per-speaker memory spec, section 1): its memory of the turns above, in its own terms.
// {actor} is the NPC's name.
inline constexpr std::string_view HOSTED_SUMMARIZE_INSTRUCTION =
    "Summarize the turns above as {actor} remembers them, for {actor}'s own memory, not for the player. Keep names, "
    "places, items, promises, secrets and who said what to whom. Leave out JSON and engine details. Plain prose, at "
    "most 200 words.";

// A hosted NPC's condense call: the memories above (RenderMemories) become one. {actor} is the NPC's name.
inline constexpr std::string_view HOSTED_CONDENSE_INSTRUCTION =
    "Condense what {actor} remembers, above, into one summary for {actor}'s own memory, not for the player. Keep names, "
    "places, items, promises, secrets, who told {actor} what, and the order of events. Plain prose, at most 250 words.";
```

- [ ] **Step 5: The speaker's L3, memories and fold**

In `src/turn/HostedSession.h`, replace the private `Entries`, `Pass` and `Upkeep` declarations with those in **Produces**, with comments:

```cpp
    // A conversation's turns with afterTurn < number <= throughTurn as transcript entries, oldest first, only those
    // `witnessId` witnessed (its speaker or a listener) when it is not empty. Rendering as before: a turn's user
    // message is the events before it (recorded since the conversation's previous turn, then its request's own, less
    // the ones its prompt left out) in parentheses and the player's line; HOSTED_TURN_CUE when it has neither. Its
    // assistant message is "<speaker> [<action id> <params>]: <line>". Names come from the turn's own observation.
    std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, int afterTurn,
                                                  int throughTurn, std::string_view witnessId);

    // One pass of the memory upkeep for `actorId` in conversation `conversationId`: the hosted prompt with
    // `transcript` as L3, `actorId`'s shown summaries as its memories (tellers by name), `hits` and the tail parts
    // `inputs` already holds, read from the store as it is now.
    FitPass Pass(std::string_view conversationId, std::string_view actorId, std::vector<context::TranscriptEntry> transcript,
                 const std::vector<RetrievalHit>& hits, context::HostedInputs inputs);

    // The shared memory upkeep for `actorId`'s memory of conversation `conversationId`, its seeds naming turn `turn`:
    // `actorId`'s scope with the hosted texts. A fold's summary is the actor's; with foldsConversation it also moves
    // the conversation's folded mark (a fold during the actor's turn), otherwise only the actor's own (a catch-up).
    MemoryUpkeep Upkeep(const std::string& conversationId, const std::string& actorId, int turn, bool foldsConversation);
```

In `src/turn/HostedSession.cpp`:

- `Open`: before anything else touches the file, after `HostedWorld world(data);`:

```cpp
    // The speaker's memories sit in the tail, capped by summary_cap, so the tail must have room for more than them.
    if (settings.budget.summaryCap >= settings.budget.tailAllowance) {
        throw core::DataError(std::format("settings: summary_cap {} must be below tail_allowance {} for a hosted run: "
                                          "the speaker's memories sit in the tail",
                                          settings.budget.summaryCap, settings.budget.tailAllowance));
    }
```

- `Entries(conversation, afterTurn, throughTurn, witnessId)`: the current body, with the condition `(turn.number <= conversation.foldedThrough) == folded` replaced by `turn.number > afterTurn && turn.number <= throughTurn && witnessed`, where `witnessed` is true when `witnessId` is empty, is the turn's speaker, or is among `turn.listenerIds`. `previous` still follows every turn of the conversation (events are placed by the conversation's turns, whoever witnessed them).
- `Pass(conversationId, actorId, transcript, hits, inputs)`:

```cpp
FitPass HostedSession::Pass(std::string_view conversationId, std::string_view actorId,
                            std::vector<context::TranscriptEntry> transcript, const std::vector<RetrievalHit>& hits,
                            context::HostedInputs inputs)
{
    // The conversation as the store holds it now: a fold since the last pass moved its marks.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Pass: the conversation is not in the store");
    // An actor's name: as a participant of this conversation, else in the observation, else its id.
    const std::vector<core::Actor> observed = HostedWorld::ActorsOf(inputs.observation);
    auto nameOf = [&conversation, &observed](std::string_view id) {
        for (const core::Participant& participant : conversation->participants) {
            if (participant.id == id) {
                return participant.name;
            }
        }
        for (const core::Actor& actor : observed) {
            if (actor.id == id) {
                return actor.name;
            }
        }
        return std::string(id);
    };
    inputs.systemText = std::string(HOSTED_SYSTEM_TEXT);
    inputs.lore = _world.Lore();
    inputs.participants = conversation->participants;
    inputs.speakerName = nameOf(actorId);
    inputs.memories.clear();
    for (const core::Summary& summary : _store.ActorSummaries(actorId, false)) {
        inputs.memories.push_back({summary.toldBy.empty() ? std::string() : nameOf(summary.toldBy), summary.text});
    }
    inputs.transcript = std::move(transcript);
    inputs.retrieved.clear();
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }

    FitPass pass;
    pass.assembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
    pass.transcript = std::move(inputs.transcript);
    return pass;
}
```

  (`HostedWorld::ActorsOf` on the maintenance placeholder's observation works: it is the last turn's observation. If `inputs.observation` can be null at a call site, guard `ActorsOf` with `inputs.observation.is_object()`.)

- `Upkeep(conversationId, actorId, turn, foldsConversation)`:

```cpp
MemoryUpkeep HostedSession::Upkeep(const std::string& conversationId, const std::string& actorId, int turn,
                                   bool foldsConversation)
{
    // The actor's name in its texts: as a participant of the conversation, else its id.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Upkeep: the conversation is not in the store");
    std::string name = actorId;
    for (const core::Participant& participant : conversation->participants) {
        if (participant.id == actorId) {
            name = participant.name;
        }
    }
    // {actor} in a hosted text is the actor's name.
    auto fill = [&name](std::string_view text) {
        std::string filled(text);
        for (std::size_t at = filled.find(ACTOR_VARIABLE); at != std::string::npos; at = filled.find(ACTOR_VARIABLE)) {
            filled.replace(at, ACTOR_VARIABLE.size(), name);
        }
        return filled;
    };

    SummaryScope scope;
    scope.shown = [this, actorId] { return _store.ActorSummaries(actorId, false); };
    scope.storeFold = [this, conversationId, actorId, foldsConversation](core::Summary summary) {
        // The actor's memory of the conversation; a fold during its turn also cuts the shared transcript there.
        summary.actorId = actorId;
        summary.conversationId = conversationId;
        if (foldsConversation) {
            _store.FoldConversation(conversationId, summary);
        } else {
            _store.RememberFold(summary);
        }
        SyncHistory();
    };
    scope.storeCondensed = [this, actorId](core::Summary condensed) {
        condensed.actorId = actorId;
        _store.CondenseActorSummaries(actorId, condensed);
        SyncHistory();
    };
    scope.foldInstruction = fill(HOSTED_SUMMARIZE_INSTRUCTION);
    scope.condenseMessage = [this, name, actorId, fill](const std::vector<core::Summary>& summaries) {
        // The memories as the actor's prompt shows them (tellers by id: a condense only needs who told what), then
        // the instruction.
        std::vector<context::MemoryLine> lines;
        for (const core::Summary& summary : summaries) {
            lines.push_back({summary.toldBy, summary.text});
        }
        return context::RenderMemories(name, lines) + "\n" + fill(HOSTED_CONDENSE_INSTRUCTION);
    };
    return MemoryUpkeep(_store, *_pBackend, _settings, std::string(HOSTED_TAIL_NAME), turn, 0, std::move(scope));
}
```

  with `constexpr std::string_view ACTOR_VARIABLE = "{actor}";` beside the other variables. (`storeFold`'s parameter is `const core::Summary&` in `SummaryScope`; take it by const reference and copy into a local before setting the fields.)

- `RunTurn`: the fit becomes

```cpp
    // The speaker's L3 is the conversation's unfolded turns it witnessed, read again each pass (a fold moves the
    // conversation's mark); a fold during this turn is the speaker's memory and cuts the shared transcript.
    auto speakerPass = [&](const std::vector<RetrievalHit>& kept) {
        const core::Conversation now = *_store.Conversation(conversation.id);
        return Pass(conversation.id, request.speakerId,
                    Entries(now, now.foldedThrough, _store.LatestHostedTurn(), request.speakerId), kept, tail);
    };
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, request.speakerId, turn, true).Fit(
        hits,
        [&](const std::vector<RetrievalHit>& kept) {
            FitPass pass = speakerPass(kept);
            while (pass.assembly.tailOverBudget && kept.empty() && !tail.events.empty()) {
                tail.events.erase(tail.events.begin());
                pass = speakerPass(kept);
            }
            return pass;
        },
        stop);
```

- `SyncHistory` uses `Entries(conversation, 0, conversation.foldedThrough, "")` for the folded turns.
- `Maintain` (until Task 5 replaces it): an ended conversation's remaining turns fold as its last turn's speaker's memory, with `Upkeep(conversation.id, lastSpeaker, latest, true)` and an `assemble` returning `Pass(conversation.id, lastSpeaker, Entries(now, now.foldedThrough, latest, lastSpeaker), {}, tail)`.

In `tests/hosted/HostedTestSupport.h`, `HostedScript::Responder` answers "Summary." for the hosted memory calls too, or every hosted fold would be taken for a speak call:

```cpp
            const std::string& last = prompt.messages.back().content;
            // Memory calls: the owned game's texts, and the hosted ones (which name the NPC).
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION ||
                last.find("Summarize the turns above as") != std::string::npos ||
                last.find("Condense what") != std::string::npos) {
                return "Summary.";
            }
```

Update the hosted tests whose expectations depended on L2 in the system message (`grep -rn "Summaries()\|summaries" tests/hosted tests/context`): they now find the speaker's memories in the tail. Update the D1 fixtures' `l3` expectations only if a fixture turn is spoken by an NPC who did not witness an earlier turn (none should be: check, and report).

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`, `"[context]"`, `"[turn]"`, `"[replay]"`, `"[hosted-server]"`.
Expected: `All tests passed` for each.

- [ ] **Step 7: Commit**

```bash
git add src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/EngineText.h src/turn/HostedSession.h src/turn/HostedSession.cpp tests/context/ tests/hosted/
git commit -m "Hosted prompts: the speaker's memories in the tail, the speaker's witnessed L3, and a fold during a turn is the speaker's memory"
```

---

