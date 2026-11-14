### Task 5: The hosted prompt layers and the default hosted texts

The context module builds a hosted prompt beside the owned one: one system message (L0 the hosted system text, L1 the host's always-on lore, the participants, L2 the run-wide summaries), the conversation's entries (L3), then the tail (retrieved snippets, the observation as compact JSON, the new events, the player's line, the instruction). Both builders lay out and measure their prompt through one shared helper, so the hosted prompt has exactly the owned prompt's budgets and flags and the shared upkeep acts on it unchanged. `EngineText` gains the default hosted texts.

**Files:**
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`
- Modify: `src/turn/EngineText.h`
- Create: `tests/context/HostedPromptTests.cpp`

**Interfaces:**
- Consumes: `context::TranscriptEntry` (Task 3); `core::Participant` (Task 2).
- Produces (`context/PromptBuilder.h`): `struct context::HostedInputs { std::string systemText; std::vector<core::LoreEntry> lore; std::vector<core::Participant> participants; std::vector<core::Summary> summaries; std::vector<TranscriptEntry> transcript; std::vector<std::string> retrieved; nlohmann::json observation; std::vector<std::string> events; std::string playerLine; std::string instruction; }`; `Assembly context::AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget, inference::LlmBackend& backend)`. The system message is `systemText + "\n\n" + ["# Lore\n" + "- <subject>: <data>\n"... + "\n"] + "# Participants\n" + "- <name> (<id>): <bio>\n" or "- <name> (<id>)\n"... + ["\n# Story so far\n" + "Turns A-B: <text>\n"...]`; the tail is `["# Relevant\n" + "- <snippet>\n"... + "\n"] + "# Observation\n<compact JSON>\n" + ["\n# New events\n" + "(<text>)\n"...] + ["\n# Player\n<playerLine>\n"] + "\n# Task\n<instruction>"`.
- Produces (`turn/EngineText.h`): `HOSTED_SYSTEM_TEXT` (no variable), `HOSTED_RESOLVE_INSTRUCTION` (`{speaker}`), `HOSTED_SPEAK_INSTRUCTION` (`{speaker}`, `{player}`), `HOSTED_TURN_CUE` (`"({speaker} speaks.)"`), all `inline constexpr std::string_view`.

- [ ] **Step 1: Write the failing tests**

Create `tests/context/HostedPromptTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/Errors.h"
#include "fakes/FakeBackend.h"
#include "turn/EngineText.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::BudgetSettings RoomyBudget()
{
    return {64, 64, 48, 512, 256, 512, 256};
}

// An L3 entry of two messages: what came before the line, then the line.
context::TranscriptEntry Entry(int turn, const std::string& user, const std::string& assistant)
{
    context::TranscriptEntry entry;
    entry.turn = turn;
    entry.messages = {{inference::Role::User, user}, {inference::Role::Assistant, assistant}};
    entry.foldMessages = entry.messages;
    entry.fact = user + " " + assistant;
    return entry;
}

// Every layer filled.
context::HostedInputs Inputs()
{
    context::HostedInputs inputs;
    inputs.systemText = "HOSTED SYSTEM";
    inputs.lore = {{"Whiterun", "A city.", true}, {"Saadia", "Hides from hunters.", false}};
    inputs.participants = {{"0001A6A4", "Hulda", "A barkeep."}, {"00000007", "Aela", ""}};
    inputs.summaries = {{1, 2, "They met."}};
    inputs.transcript = {Entry(3, "Aela: Hello.", "Hulda [none]: Welcome.")};
    inputs.retrieved = {"Turn 1: Aela: A room, please."};
    inputs.observation = {{"location", "inn"}};
    inputs.events = {"Door opens."};
    inputs.playerLine = "Aela: Follow me.";
    inputs.instruction = "TASK";
    return inputs;
}

} // namespace

TEST_CASE("AssembleHostedPrompt lays out the hosted layers, most stable first", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleHostedPrompt(Inputs(), RoomyBudget(), backend);
    const inference::Prompt& prompt = assembly.prompt;
    REQUIRE(prompt.messages.size() == 4);
    CHECK(prompt.stableCount == 3);

    // L0, L1 (always-on lore only), the participants in join order, L2.
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(prompt.messages[0].content == "HOSTED SYSTEM\n\n"
                                        "# Lore\n- Whiterun: A city.\n\n"
                                        "# Participants\n- Hulda (0001A6A4): A barkeep.\n- Aela (00000007)\n\n"
                                        "# Story so far\nTurns 1-2: They met.\n");
    // L3.
    CHECK(prompt.messages[1].role == inference::Role::User);
    CHECK(prompt.messages[1].content == "Aela: Hello.");
    CHECK(prompt.messages[2].role == inference::Role::Assistant);
    CHECK(prompt.messages[2].content == "Hulda [none]: Welcome.");
    // The tail.
    CHECK(prompt.messages[3].role == inference::Role::User);
    CHECK(prompt.messages[3].content == "# Relevant\n- Turn 1: Aela: A room, please.\n\n"
                                        "# Observation\n{\"location\":\"inn\"}\n\n"
                                        "# New events\n(Door opens.)\n\n"
                                        "# Player\nAela: Follow me.\n\n"
                                        "# Task\nTASK");
}

TEST_CASE("AssembleHostedPrompt leaves out empty optional layers", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    context::HostedInputs inputs = Inputs();
    inputs.lore = {{"Saadia", "Hides from hunters.", false}}; // Retrieved only: never in L1.
    inputs.participants = {{"0001A6A4", "Hulda", "A barkeep."}};
    inputs.summaries.clear();
    inputs.transcript.clear();
    inputs.retrieved.clear();
    inputs.events.clear();
    inputs.playerLine.clear();
    const context::Assembly assembly = context::AssembleHostedPrompt(inputs, RoomyBudget(), backend);
    REQUIRE(assembly.prompt.messages.size() == 2);
    CHECK(assembly.prompt.stableCount == 1);
    CHECK(assembly.prompt.messages[0].content == "HOSTED SYSTEM\n\n# Participants\n- Hulda (0001A6A4): A barkeep.\n");
    CHECK(assembly.prompt.messages[1].content == "# Observation\n{\"location\":\"inn\"}\n\n# Task\nTASK");

    // The system text, a participant and the instruction are required: their absence is an engine bug.
    inputs.participants.clear();
    CHECK_THROWS_AS(context::AssembleHostedPrompt(inputs, RoomyBudget(), backend), core::ContractError);
}

TEST_CASE("AssembleHostedPrompt counts and budgets like the resolve prompt", "[context][hosted]")
{
    const core::BudgetSettings budget = RoomyBudget();
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleHostedPrompt(Inputs(), budget, backend);
    CHECK(assembly.tokens.engineRules == backend.CountTokens("HOSTED SYSTEM"));
    CHECK(assembly.tokens.world == backend.CountTokens("# Lore\n- Whiterun: A city.\n\n# Participants\n"
                                                       "- Hulda (0001A6A4): A barkeep.\n- Aela (00000007)\n"));
    CHECK(assembly.tokens.summary == backend.CountTokens("# Story so far\nTurns 1-2: They met.\n"));
    CHECK(assembly.tokens.transcript ==
          backend.CountTokens("Aela: Hello.") + backend.CountTokens("Hulda [none]: Welcome."));
    CHECK(assembly.tokens.tail == backend.CountTokens(assembly.prompt.messages[3].content));
    CHECK(assembly.tokens.total == backend.CountPromptTokens(assembly.prompt));
    const int systemCost = backend.CountTokens(assembly.prompt.messages[0].content) + 4;
    CHECK(assembly.transcriptBudget == 8192 - 64 - 64 - 48 - 512 - systemCost);
    CHECK(assembly.transcriptCost ==
          backend.CountTokens("Aela: Hello.") + 4 + backend.CountTokens("Hulda [none]: Welcome.") + 4);
    CHECK(assembly.Fits());

    // The flags the shared upkeep acts on.
    context::HostedInputs longTail = Inputs();
    longTail.playerLine = "Aela: " + std::string(4000, 'y');
    CHECK(context::AssembleHostedPrompt(longTail, budget, backend).tailOverBudget);
    context::HostedInputs longSummary = Inputs();
    longSummary.summaries.front().text = std::string(2000, 's');
    CHECK(context::AssembleHostedPrompt(longSummary, budget, backend).summaryOverBudget);
    context::HostedInputs longTranscript = Inputs();
    longTranscript.transcript.front().messages.back().content = std::string(40000, 'x');
    const context::Assembly over = context::AssembleHostedPrompt(longTranscript, budget, backend);
    CHECK(over.transcriptOverBudget);
    CHECK_FALSE(over.Fits());

    // Lore and participants over world_cap are the host's data error, the system text over engine_rules_cap an engine
    // bug, and a context with no room for L3 a data error.
    core::BudgetSettings smallWorld = budget;
    smallWorld.worldCap = 2;
    CHECK_THROWS_AS(context::AssembleHostedPrompt(Inputs(), smallWorld, backend), core::DataError);
    CHECK_THROWS_WITH(context::AssembleHostedPrompt(Inputs(), smallWorld, backend),
                      ContainsSubstring("host lore and participants use"));
    core::BudgetSettings smallRules = budget;
    smallRules.engineRulesCap = 1;
    CHECK_THROWS_AS(context::AssembleHostedPrompt(Inputs(), smallRules, backend), core::ContractError);
    test::FakeBackend tiny(600);
    CHECK_THROWS_WITH(context::AssembleHostedPrompt(Inputs(), budget, tiny), ContainsSubstring("no room for the transcript"));
}

TEST_CASE("A join changes the hosted prompt only from the participants layer on", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    context::HostedInputs joinedInputs = Inputs();
    joinedInputs.participants.push_back({"0001B07C", "Saadia", "Serves at the inn."});
    const context::Assembly before = context::AssembleHostedPrompt(Inputs(), RoomyBudget(), backend);
    const context::Assembly joined = context::AssembleHostedPrompt(joinedInputs, RoomyBudget(), backend);
    const std::string& a = before.prompt.messages[0].content;
    const std::string& b = joined.prompt.messages[0].content;

    // The first difference lies past the start of the participants layer: L0 and L1 stay cached.
    const std::size_t participantsAt = a.find("# Participants\n");
    REQUIRE(participantsAt != std::string::npos);
    std::size_t same = 0;
    while (same < a.size() && same < b.size() && a[same] == b[same]) {
        ++same;
    }
    CHECK(a != b);
    CHECK(same > participantsAt);
    CHECK_THAT(b, ContainsSubstring("- Aela (00000007)\n- Saadia (0001B07C): Serves at the inn.\n"));

    // L3 and the tail are as they were.
    REQUIRE(joined.prompt.messages.size() == before.prompt.messages.size());
    for (std::size_t i = 1; i < before.prompt.messages.size(); ++i) {
        CHECK(joined.prompt.messages[i].content == before.prompt.messages[i].content);
    }
}

TEST_CASE("A hosted prompt keeps its stable prefix when the conversation grows by a turn", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    context::HostedInputs nextInputs = Inputs();
    nextInputs.transcript.push_back(Entry(4, "(Door opens.)\nAela: Follow me.", "Hulda [follow]: Lead the way."));
    nextInputs.events.clear();
    nextInputs.playerLine = "Aela: Faster.";
    const context::Assembly first = context::AssembleHostedPrompt(Inputs(), RoomyBudget(), backend);
    const context::Assembly next = context::AssembleHostedPrompt(nextInputs, RoomyBudget(), backend);
    REQUIRE(next.prompt.stableCount == first.prompt.stableCount + 2);
    for (std::size_t i = 0; i < first.prompt.stableCount; ++i) {
        CHECK(next.prompt.messages[i].role == first.prompt.messages[i].role);
        CHECK(next.prompt.messages[i].content == first.prompt.messages[i].content);
    }
}

TEST_CASE("The default hosted texts are generic, fit their cap and use only the engine's variables",
          "[context][hosted]")
{
    // L0 has no variable, so it never changes between turns.
    CHECK(turn::HOSTED_SYSTEM_TEXT.find('{') == std::string_view::npos);
    CHECK(turn::HOSTED_RESOLVE_INSTRUCTION.find("{speaker}") != std::string_view::npos);
    CHECK(turn::HOSTED_SPEAK_INSTRUCTION.find("{speaker}") != std::string_view::npos);
    CHECK(turn::HOSTED_SPEAK_INSTRUCTION.find("{player}") != std::string_view::npos);
    CHECK(turn::HOSTED_TURN_CUE == "({speaker} speaks.)");
    // Every brace opens one of the two variables the engine fills.
    for (const std::string_view text :
         {turn::HOSTED_RESOLVE_INSTRUCTION, turn::HOSTED_SPEAK_INSTRUCTION, turn::HOSTED_TURN_CUE}) {
        for (std::size_t at = text.find('{'); at != std::string_view::npos; at = text.find('{', at + 1)) {
            const std::string_view rest = text.substr(at);
            CHECK((rest.starts_with("{speaker}") || rest.starts_with("{player}")));
        }
    }
    // L0 fits settings.example.json's engine_rules_cap (1024) as the fast suite counts tokens.
    test::FakeBackend backend(8192);
    CHECK(backend.CountTokens(turn::HOSTED_SYSTEM_TEXT) <= 1024);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `'HostedInputs': is not a member of 'morpheus::context'` and `'HOSTED_SYSTEM_TEXT': is not a member of 'morpheus::turn'`.

- [ ] **Step 3: Write the default hosted texts**

In `src/turn/EngineText.h`, before `} // namespace morpheus::turn`, insert:

```cpp

// L0 of a hosted run (hosted-world spec, "Default hosted texts"): voicing characters in a world the host simulates,
// and the output rules. Generic: it names no game. It has no variable, so L0 never changes between turns. Sub-project
// F lets the host replace it.
inline constexpr std::string_view HOSTED_SYSTEM_TEXT = R"(# Engine
You voice the characters of a role-playing game that a host game runs. The host simulates the world: its places, its people, their items and the outcome of everything anyone does. You never decide outcomes. Each turn you speak for one character, the speaker, and choose the action they try.

Every turn has two steps.

Step 1, act. Reply with one JSON object and nothing else: "action" is one of the actions the turn allows, and "params" holds that action's parameters. A parameter that names an actor takes the actor's id from the observation. Choose what the speaker would do now. The host decides whether it happens; its outcome arrives later as an event.

Step 2, speak. Write only what the speaker says aloud: no name in front, no quotation marks, no narration and no description of actions. Stay in character and in the world. One to three sentences.

The conversation shows each line after the name of whoever said it, with the action the speaker tried in brackets. Events the host reports are in parentheses. The observation is the host's current view of the scene, as JSON.)";

// The task at the end of a hosted resolve prompt's tail. {speaker} and {player} are the speaker's and the player's
// names.
inline constexpr std::string_view HOSTED_RESOLVE_INSTRUCTION =
    "Step 1: choose the action {speaker} tries now. Reply with the JSON object only.";

// The user message the speak call appends after the resolve output; it must fit narrate_allowance.
inline constexpr std::string_view HOSTED_SPEAK_INSTRUCTION =
    "Step 2: write what {speaker} says now. Spoken words only, in character. {player} is the player's character.";

// L3's user message for a turn that had no events and no player text before the speaker's line.
inline constexpr std::string_view HOSTED_TURN_CUE = "({speaker} speaks.)";
```

- [ ] **Step 4: Write the hosted prompt builder**

Replace the whole of `src/context/PromptBuilder.h` with:

```cpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/Transcript.h"
#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"

namespace morpheus::context {

// What goes into one resolve prompt, by layer (architecture spec, "Context layout and KV cache"). The world
// (L1) is passed separately.
struct ContextInputs {
    std::string engineRules = {};                  // L0: engine rules and the output format.
    std::vector<core::Summary> summaries = {};     // L2: folded turns, oldest first.
    std::vector<TranscriptEntry> transcript = {};  // L3: unfolded turns, oldest first, rendered (TranscriptEntries).
    nlohmann::json state = nullptr;                // T1: current state (validated).
    std::vector<std::string> retrieved = {};       // T2: formatted lore/history snippets.
    std::string playerInput = {};                  // T3
    std::string instruction = {};                  // T4: the resolve task instruction.
};

// What goes into one hosted resolve prompt, by layer (hosted-world spec, "The prompt"), most stable first, so a new
// turn adds to the end of the cached part.
struct HostedInputs {
    std::string systemText = {};                      // L0: the hosted system text.
    std::vector<core::LoreEntry> lore = {};           // L1: the host's lore; only the always-on entries are shown.
    std::vector<core::Participant> participants = {}; // The participants, in join order: name, id and bio.
    std::vector<core::Summary> summaries = {};        // L2: run-wide summaries of folded turns, oldest first.
    std::vector<TranscriptEntry> transcript = {};     // L3: this conversation's unfolded turns, rendered.
    std::vector<std::string> retrieved = {};          // Tail: lore and history snippets.
    nlohmann::json observation = nullptr;             // Tail: shown as compact JSON.
    std::vector<std::string> events = {};             // Tail: the new events' texts, in order.
    std::string playerLine = {};                      // Tail: "<player name>: <text>", or empty.
    std::string instruction = {};                     // Tail: the resolve instruction.
};

// Exact token counts. Layer counts are raw text (no template tokens), for the inspector; `total` is the only
// figure that includes chat-template tokens (the whole templated prompt).
struct LayerTokens {
    int engineRules = 0; // L0.
    int world = 0;       // L1; in a hosted prompt, the host's always-on lore and the participants.
    int summary = 0;
    int transcript = 0;
    int tail = 0;
    int total = 0;
};

struct Assembly {
    inference::Prompt prompt = {};
    LayerTokens tokens = {};
    int transcriptBudget = 0;          // Tokens L3 may add to the templated prompt, with the summary counted at most at summary_cap (always positive).
    int transcriptCost = 0;            // Tokens L3 adds to the templated prompt, template tokens included.
    bool transcriptOverBudget = false; // Fold old turns into the summary and assemble again.
    bool summaryOverBudget = false;    // The summary itself must be condensed.
    bool tailOverBudget = false;       // The tail (state or observation, retrieval, input and instruction) exceeds tail_allowance.
    // The full templated prompt plus both generations exceeds the context: shrink the tail or fold.
    bool contextOverflow = false;

    bool Fits() const
    {
        return !transcriptOverBudget && !summaryOverBudget && !tailOverBudget && !contextOverflow;
    }
};

// Builds the resolve prompt: one system message (L0 + L1 + L2), the transcript (L3), then one user message with the
// volatile tail (T1-T4). L1 is the world's rules, game reference (RenderWorldReference) and always-on lore.
// prompt.stableCount marks the end of L3. Throws ContractError if L0 exceeds engine_rules_cap (engine text too
// long) and DataError if L1 exceeds world_cap (the world is too big) or if the context leaves no room for the
// transcript after the system message (its summary counted at most at summary_cap), the tail allowance, the narrate
// allowance and both generations, thinking blocks included (folding cannot fix that). Other overruns, an over-cap
// summary among them, are flags for the turn layer to act on.
//
// The narrate call appends two messages that are not part of this prompt: the resolve output (assistant, up to
// resolveMaxTokens) and the resolution record + instruction (user). budget.narrateAllowance is reserved for the
// user message's text: transcriptBudget excludes it. contextOverflow counts it together with the template
// overhead of both messages (assistant and user), each approximated by the cost of an empty one-message prompt.
Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend);

// Builds a hosted resolve prompt: one system message (L0, then L1 the always-on lore, then the participants as
// "- <name> (<id>): <bio>" or "- <name> (<id>)", then L2), the conversation's entries (L3), then one user message with
// the tail (retrieved snippets, the observation as compact JSON, the new events in parentheses, the player's line,
// the instruction). prompt.stableCount marks the end of L3. A join or a leave changes the prompt from the
// participants on; a new turn only adds entries before the tail. Budgets and flags are AssembleResolvePrompt's, with
// the speak call in the narrate call's place: tokens.world counts the lore and the participants together against
// world_cap (a DataError over it: the host's data); the system text over engine_rules_cap is a ContractError.
Assembly AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget,
                              inference::LlmBackend& backend);

// Turns a resolve prompt into the narrate prompt by appending the resolve output (assistant) and the resolution
// record with the narrate instruction (user). It only appends, so the whole resolve prompt stays cached.
// The appended user message must stay within budget.narrate_allowance; the turn layer checks this.
void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction);

} // namespace morpheus::context
```

Replace the whole of `src/context/PromptBuilder.cpp` with:

```cpp
#include "context/PromptBuilder.h"

#include <algorithm>
#include <format>
#include <utility>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "context/WorldReference.h"
#include "core/Errors.h"

namespace morpheus::context {

namespace {

// Lays out a prompt into `assembly`: one system message with `systemText`, the messages of the transcript entries
// (the stable prefix ends after them), then one user message with `tail`. Then measures it against `budget`: the
// layer counts the caller has not set (tail, transcript, total), the transcript's budget and cost, and every flag. The
// caller has set tokens.summary: an over-cap summary is counted at most at its cap. Throws DataError when the context
// leaves no room for the transcript (folding cannot fix that).
void LayOut(Assembly& assembly, std::string systemText, const std::vector<TranscriptEntry>& transcript,
            std::string tail, const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    // The fixed part of the prompt: the system message and the tail. Counting it, with and without the tail,
    // gives the template-inclusive costs the transcript budget needs.
    inference::Prompt fixed;
    fixed.messages.push_back({inference::Role::System, std::move(systemText)});
    fixed.messages.push_back({inference::Role::User, std::move(tail)});

    // Exact counts with the model's tokenizer. Layer counts are raw text; only `total` includes template tokens.
    LayerTokens& tokens = assembly.tokens;
    tokens.tail = backend.CountTokens(fixed.messages.back().content);
    const int fixedTotal = backend.CountPromptTokens(fixed); // System message and tail, no transcript.
    inference::Message tailMessage = std::move(fixed.messages.back());
    fixed.messages.pop_back();
    const int systemCost = backend.CountPromptTokens(fixed); // The system message alone, templated.

    // Messages: stable prefix (system, transcript), then the tail.
    inference::Prompt& prompt = assembly.prompt;
    prompt.messages.push_back(std::move(fixed.messages.front()));
    for (const TranscriptEntry& entry : transcript) {
        for (const inference::Message& message : entry.messages) {
            tokens.transcript += backend.CountTokens(message.content);
            prompt.messages.push_back(message);
        }
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back(std::move(tailMessage));
    tokens.total = backend.CountPromptTokens(prompt);
    assembly.transcriptCost = tokens.total - fixedTotal;

    assembly.summaryOverBudget = tokens.summary > budget.summaryCap;
    assembly.tailOverBudget = tokens.tail > budget.tailAllowance;

    // L3 gets what the templated system message, the tail allowance, the narrate allowance and both generations
    // leave, with the summary counted at most at its cap: an over-cap summary is a flag (condense it), not a
    // settings error. A generation is a call's thinking block plus its answer. Reserving both thinking blocks is
    // conservative: the resolve call's block is not part of the narrate prompt, so the two never share a context.
    const int summaryExcess = std::max(0, tokens.summary - budget.summaryCap);
    assembly.transcriptBudget = backend.ContextSize() - budget.ResolveGenerationTokens() -
                                budget.NarrateGenerationTokens() - budget.narrateAllowance - budget.tailAllowance -
                                (systemCost - summaryExcess);
    if (assembly.transcriptBudget <= 0) {
        // Folding turns cannot fix this: the settings or the world leave L3 nothing, so it is not a flag.
        throw core::DataError(std::format("context_size {} leaves no room for the transcript after the system "
                                          "layers ({} tokens), the tail allowance, the narrate allowance and both "
                                          "generations",
                                          backend.ContextSize(), systemCost - summaryExcess));
    }
    assembly.transcriptOverBudget = assembly.transcriptCost > assembly.transcriptBudget;

    // Final safety check on the whole templated prompt, as seen by the narrate call: the prompt, the narrate
    // message (its template overhead plus the allowance for its text), the resolve output's assistant message
    // (template overhead only: its text is the resolve generation) and both generations. Each overhead is
    // approximated by the cost of an empty one-message prompt.
    inference::Prompt emptyUser;
    emptyUser.messages.push_back({inference::Role::User, ""});
    const int narrateMessageOverhead = backend.CountPromptTokens(emptyUser);
    inference::Prompt emptyAssistant;
    emptyAssistant.messages.push_back({inference::Role::Assistant, ""});
    const int resolveOutputOverhead = backend.CountPromptTokens(emptyAssistant);
    assembly.contextOverflow = tokens.total + budget.narrateAllowance + narrateMessageOverhead +
                                   resolveOutputOverhead + budget.ResolveGenerationTokens() +
                                   budget.NarrateGenerationTokens() >
                               backend.ContextSize();
}

} // namespace

Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.engineRules.empty() && !inputs.playerInput.empty() && !inputs.instruction.empty(),
                     "AssembleResolvePrompt: engine rules, player input and instruction are required");

    // L1: the world's rules, its game reference, then its always-on lore.
    std::string worldText = "# World\n" + world.rules;
    if (!worldText.ends_with('\n')) {
        worldText += '\n';
    }
    worldText += "\n" + RenderWorldReference(world);
    std::string lore;
    for (const core::LoreEntry& entry : world.lore) {
        if (entry.alwaysOn) {
            lore += std::format("- {}: {}\n", entry.subject, entry.data);
        }
    }
    if (!lore.empty()) {
        worldText += "# Lore\n" + lore;
    }

    // L2: folded summaries, oldest first.
    std::string summaryText;
    if (!inputs.summaries.empty()) {
        summaryText = "# Story so far\n";
        for (const core::Summary& summary : inputs.summaries) {
            summaryText += std::format("Turns {}-{}: {}\n", summary.fromTurn, summary.toTurn, summary.text);
        }
    }

    // T1-T4: rebuilt every call, so it goes last.
    std::string tail = "# Current state\n" + RenderStateView(inputs.state);
    if (!inputs.retrieved.empty()) {
        tail += "\n# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
    }
    tail += "\n# Player\n" + inputs.playerInput + "\n\n# Task\n" + inputs.instruction;

    // The system layers' counts, then their caps: L0 is engine text (a bug if too long), L1 is world data (the
    // author must trim it).
    Assembly assembly;
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.engineRules);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("engine rules use {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("world '{}': rules and always-on lore use {} tokens, over world_cap {}",
                                          world.id, tokens.world, budget.worldCap));
    }
    LayOut(assembly, inputs.engineRules + "\n\n" + worldText + summaryText, inputs.transcript, std::move(tail), budget,
           backend);
    return assembly;
}

Assembly AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget,
                              inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.systemText.empty() && !inputs.participants.empty() && !inputs.instruction.empty(),
                     "AssembleHostedPrompt: the system text, a participant and the instruction are required");

    // L1, then the participants: the host's always-on lore, then who takes part, in join order. A join or a leave
    // changes the prompt from the participants on; the lore before them stays cached.
    std::string lore;
    for (const core::LoreEntry& entry : inputs.lore) {
        if (entry.alwaysOn) {
            lore += std::format("- {}: {}\n", entry.subject, entry.data);
        }
    }
    std::string worldText = lore.empty() ? std::string() : "# Lore\n" + lore + "\n";
    worldText += "# Participants\n";
    for (const core::Participant& participant : inputs.participants) {
        worldText += participant.bio.empty()
                         ? std::format("- {} ({})\n", participant.name, participant.id)
                         : std::format("- {} ({}): {}\n", participant.name, participant.id, participant.bio);
    }

    // L2: run-wide summaries of folded turns, oldest first.
    std::string summaryText;
    if (!inputs.summaries.empty()) {
        summaryText = "# Story so far\n";
        for (const core::Summary& summary : inputs.summaries) {
            summaryText += std::format("Turns {}-{}: {}\n", summary.fromTurn, summary.toTurn, summary.text);
        }
    }

    // The tail, rebuilt every turn, so it goes last: retrieved snippets, the observation, the new events, the
    // player's line and the instruction. The replace handler keeps a string that is not UTF-8 from throwing: the
    // observation is the host's data.
    std::string tail;
    if (!inputs.retrieved.empty()) {
        tail += "# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
        tail += "\n";
    }
    tail += "# Observation\n" + inputs.observation.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    if (!inputs.events.empty()) {
        tail += "\n# New events\n";
        for (const std::string& event : inputs.events) {
            tail += "(" + event + ")\n";
        }
    }
    if (!inputs.playerLine.empty()) {
        tail += "\n# Player\n" + inputs.playerLine + "\n";
    }
    tail += "\n# Task\n" + inputs.instruction;

    // The system layers' counts, then their caps: L0 is engine text (a bug if too long), the lore and the
    // participants are the host's data.
    Assembly assembly;
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.systemText);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("the hosted system text uses {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("host lore and participants use {} tokens, over world_cap {}", tokens.world,
                                          budget.worldCap));
    }
    const std::string separator = summaryText.empty() ? std::string() : "\n";
    LayOut(assembly, inputs.systemText + "\n\n" + worldText + separator + summaryText, inputs.transcript,
           std::move(tail), budget, backend);
    return assembly;
}

void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction)
{
    MORPHEUS_REQUIRE(!prompt.messages.empty() && prompt.messages.back().role == inference::Role::User,
                     "ExtendForNarrate: expects a resolve prompt ending with the user tail");
    prompt.messages.push_back({inference::Role::Assistant, resolveOutput});
    prompt.messages.push_back({inference::Role::User, resolutionAndInstruction});
}

} // namespace morpheus::context
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[context]"`
Expected: builds with no warnings; `All tests passed` (the owned prompt tests unchanged, the six new ones).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 463` (457 + 6), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
FILES="src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/EngineText.h tests/context/HostedPromptTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Add the hosted prompt layers (context::AssembleHostedPrompt) and the default hosted texts" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

