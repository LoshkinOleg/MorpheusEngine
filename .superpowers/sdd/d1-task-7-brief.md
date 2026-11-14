### Task 7: `HostedSession::RunTurn`

A hosted turn: check the request (nothing stored, no model call before it passes), retrieve from lore and the run's folded turns, resolve under the proposal grammar through the shared fit loop, adjudicate and report the action, speak streamed on a prompt that extends the resolve prompt, commit. A cancel at any step commits nothing. `Session`'s call telemetry becomes the public `turn::CallTelemetry` so both sessions write it the same way.

**Files:**
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Modify: `tests/hosted/HostedTestSupport.h`
- Create: `tests/hosted/HostedTurnTests.cpp`

**Interfaces:**
- Consumes: `MemoryUpkeep`, `FitPass`, `TurnError` (Task 3); `HostedWorld::ProposalSchema`, `ProposalGrammar`, `Adjudicate`, `CheckEvents`, `ActionIds`, `Observation` (Task 4); `context::AssembleHostedPrompt`, `HostedInputs`, `HOSTED_SYSTEM_TEXT`, `HOSTED_RESOLVE_INSTRUCTION`, `HOSTED_SPEAK_INSTRUCTION` (Task 5); `HostedSession` and its private `OpenConversation`, `Entries`, `SyncHistory` (Task 6).
- Produces (`turn/Session.h`): `nlohmann::json turn::CallTelemetry(const inference::Completion& completion, const inference::GenerationSpec& spec)`.
- Produces (`turn/HostedSession.h`): `struct turn::HostedTurnRequest { std::string conversationId; std::string speakerId; std::optional<std::string> playerText; std::vector<nlohmann::json> events; nlohmann::json observation; std::optional<std::vector<std::string>> allowedActions; }`; `struct turn::HostedCallbacks { std::function<void(const core::Adjudication&)> onAction; inference::TokenCallback onToken; }`; `std::optional<core::HostedTurn> HostedSession::RunTurn(const HostedTurnRequest& request, const HostedCallbacks& callbacks, std::stop_token stop)`; private `FitPass Pass(std::string_view conversationId, const std::vector<RetrievalHit>& hits, context::HostedInputs inputs)` and `MemoryUpkeep Upkeep(const std::string& conversationId, int turn)`.
- Produces (`tests/hosted/HostedTestSupport.h`): `struct test::HostedScript { std::vector<std::string> actions; std::vector<std::string> lines; ...; ScriptedBackend::Responder Responder(); }`; `turn::HostedTurnRequest test::Request(const std::string& conversationId, const std::string& speakerId, const std::string& playerText)`.

- [ ] **Step 1: Write the failing tests**

In `tests/hosted/HostedTestSupport.h`, replace

```cpp
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Settings.h"
#include "turn/HostedWorld.h"
```

with

```cpp
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"
```

and before `} // namespace morpheus::test`, insert:

```cpp

// A scripted model for hosted turns. Resolve calls (they carry the proposal grammar) get `actions` in order, speak
// calls (no grammar) get `lines` in order, and fold and condense calls "Summary.". Running out of replies is a test
// bug. The script must outlive the backends that call it.
struct HostedScript {
    std::vector<std::string> actions = {};
    std::vector<std::string> lines = {};
    std::size_t acted = 0;
    std::size_t spoken = 0;

    ScriptedBackend::Responder Responder()
    {
        return [this](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
            const std::string& last = prompt.messages.back().content;
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
                return "Summary.";
            }
            if (!spec.grammar.empty()) {
                MORPHEUS_REQUIRE(acted < actions.size(), "HostedScript: no action left");
                return actions[acted++];
            }
            MORPHEUS_REQUIRE(spoken < lines.size(), "HostedScript: no line left");
            return lines[spoken++];
        };
    }
};

// A request in conversation `conversationId`: `speakerId` speaks after `playerText` (no player text when it is
// empty), in observation.json, with every action of the catalog allowed.
inline turn::HostedTurnRequest Request(const std::string& conversationId, const std::string& speakerId,
                                       const std::string& playerText)
{
    turn::HostedTurnRequest request;
    request.conversationId = conversationId;
    request.speakerId = speakerId;
    if (!playerText.empty()) {
        request.playerText = playerText;
    }
    request.observation = FixtureObservation();
    return request;
}
```

Create `tests/hosted/HostedTurnTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "grammar/Check.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

const std::string FOLLOW = R"({"action":"follow","params":{}})";
const std::string NONE = R"({"action":"none","params":{}})";

// A hosted run in a fresh file for test `name`, on `backend`.
turn::HostedSession OpenRun(const std::string& name, test::ScriptedBackend& backend)
{
    return turn::HostedSession::Open(test::HostedRunFile(name), test::FixtureHostData(), test::HostedSettings(), 42,
                                     backend);
}

} // namespace

TEST_CASE("A hosted turn resolves, adjudicates, speaks streamed and commits its record", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW};
    script.lines = {"Lead the way, then."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_turn", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    std::vector<std::string> order;
    std::string streamed;
    turn::HostedCallbacks callbacks;
    callbacks.onAction = [&](const core::Adjudication& adjudication) { order.push_back("action " + adjudication.actionId); };
    callbacks.onToken = [&](std::string_view piece) {
        if (order.back() != "token") {
            order.push_back("token");
        }
        streamed += piece;
    };
    const std::optional<core::HostedTurn> record =
        session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), callbacks, {});
    REQUIRE(record);
    CHECK(order == std::vector<std::string>{"action follow", "token"}); // The action before the line.
    CHECK(streamed == "Lead the way, then.");

    // The record of section 2, step 4.
    CHECK(record->number == 1);
    CHECK(record->conversationId == id);
    CHECK(record->speakerId == test::HULDA);
    CHECK(record->listenerIds == std::vector<std::string>{test::AELA}); // Saadia is present but takes no part.
    CHECK(record->playerText == std::optional<std::string>("Hulda, follow me."));
    CHECK(record->events.empty());
    CHECK(record->observation == test::FixtureObservation());
    CHECK(record->resolveOutput == FOLLOW);
    CHECK(record->adjudication.requested);
    CHECK(record->adjudication.actionId == "follow");
    CHECK(record->adjudication.params == nlohmann::json::object());
    CHECK(record->line == "Lead the way, then.");
    CHECK(record->timings.contains("resolve"));
    CHECK(record->timings.contains("speak"));
    CHECK(record->timings.contains("retrieval"));
    const std::vector<core::HostedTurn> turns = session.Turns(id);
    REQUIRE(turns.size() == 1);
    CHECK(turns[0].line == record->line);

    // The resolve call runs under the proposal grammar with resolve_max_tokens; the speak call extends its prompt with
    // the resolve output and the speak instruction, and has no grammar.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 2);
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Participants participants = session.Conversation(id)->participants;
    CHECK(calls[0].spec.grammar == world.ProposalGrammar(world.Observe(test::FixtureObservation()), participants,
                                                         world.ActionIds(), test::HULDA));
    CHECK(calls[0].spec.maxTokens == 64);
    const std::string& tail = calls[0].prompt.messages.back().content;
    CHECK(tail.ends_with("# Task\nStep 1: choose the action Hulda tries now. Reply with the JSON object only."));
    CHECK_THAT(tail, ContainsSubstring("# Player\nAela: Hulda, follow me.\n"));
    const inference::Prompt& speak = calls[1].prompt;
    REQUIRE(speak.messages.size() == calls[0].prompt.messages.size() + 2);
    for (std::size_t i = 0; i < calls[0].prompt.messages.size(); ++i) {
        CHECK(speak.messages[i].content == calls[0].prompt.messages[i].content);
    }
    CHECK(speak.messages[speak.messages.size() - 2].role == inference::Role::Assistant);
    CHECK(speak.messages[speak.messages.size() - 2].content == FOLLOW);
    CHECK(speak.messages.back().content ==
          "Step 2: write what Hulda says now. Spoken words only, in character. Aela is the player's character.");
    CHECK(calls[1].spec.grammar.empty());
    CHECK(calls[1].spec.maxTokens == 256);
    CHECK(session.LastAssembly().prompt.messages.size() == calls[0].prompt.messages.size());
}

TEST_CASE("A refused action is kept with its reasons, and the turn speaks with no action", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"wait","params":{},"intent":"rest"})", NONE};
    script.lines = {"As you wish.", "Still here."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_refused", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> refused = session.RunTurn(test::Request(id, test::HULDA, "Wait here."), {}, {});
    REQUIRE(refused);
    CHECK_FALSE(refused->adjudication.requested);
    CHECK(refused->adjudication.reasons ==
          std::vector<std::string>{"/intent: unexpected key 'intent'; the keys are 'action', 'params'"});
    CHECK(refused->resolveOutput == R"({"action":"wait","params":{},"intent":"rest"})");
    CHECK(refused->line == "As you wish.");
    CHECK(refused->timings.at("resolve").at("refused") == nlohmann::json(refused->adjudication.reasons));

    // In L3 the refused turn shows no bracket.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Are you there?"), {}, {}));
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    REQUIRE(prompt.stableCount == 3);
    CHECK(prompt.messages[1].content == "Aela: Wait here.");
    CHECK(prompt.messages[2].content == "Hulda: As you wish.");
}

TEST_CASE("A turn between NPCs needs no player text", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {NONE, FOLLOW, NONE};
    script.lines = {"Busy night.", "I will help you.", "Quiet again."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_npc", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> first = session.RunTurn(test::Request(id, test::SAADIA, ""), {}, {});
    REQUIRE(first);
    CHECK_FALSE(first->playerText);
    CHECK(first->listenerIds == std::vector<std::string>{test::HULDA});
    CHECK_THAT(backend.Calls().at(0).prompt.messages.back().content, !ContainsSubstring("# Player"));

    // Hulda answers: Saadia's turn shows in L3 with the cue where a player's line would be.
    const std::optional<core::HostedTurn> second = session.RunTurn(test::Request(id, test::HULDA, ""), {}, {});
    REQUIRE(second);
    CHECK(second->listenerIds == std::vector<std::string>{test::SAADIA});
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    REQUIRE(prompt.stableCount == 3);
    CHECK(prompt.messages[1].content == "(Saadia speaks.)");
    CHECK(prompt.messages[2].content == "Saadia [none]: Busy night.");

    // A player text of only spaces and control characters is no player text.
    const std::optional<core::HostedTurn> third = session.RunTurn(test::Request(id, test::SAADIA, " \t \n"), {}, {});
    REQUIRE(third);
    CHECK_FALSE(third->playerText);
}

TEST_CASE("The prompt is byte-identical across turns of one conversation up to the tail", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, NONE, NONE};
    script.lines = {"Lead the way, then.", "Right behind you.", "Here we are."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_prefix", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    std::vector<inference::Prompt> prompts;
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    session.RecordEvents(id, {R"({"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true,
                                 "text": "Hulda is now following Aela."})"_json});
    turn::HostedTurnRequest second = test::Request(id, test::HULDA, "To the gate.");
    second.events = {R"({"type": "ambient", "text": "The door creaks."})"_json};
    REQUIRE(session.RunTurn(second, {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Stop here."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);

    // Each prompt's stable part (the system message and L3) is the start of the next one's, message for message: a
    // new turn only adds to the end of the cached part.
    for (std::size_t k = 0; k + 1 < prompts.size(); ++k) {
        INFO("turns " << k + 1 << " and " << k + 2);
        const inference::Prompt& before = prompts[k];
        const inference::Prompt& after = prompts[k + 1];
        REQUIRE(after.stableCount == before.stableCount + 2);
        for (std::size_t i = 0; i < before.stableCount; ++i) {
            CHECK(after.messages[i].role == before.messages[i].role);
            CHECK(after.messages[i].content == before.messages[i].content);
        }
    }

    // The second turn's tail holds both new events; the third's L3 shows them before the player's line.
    CHECK_THAT(prompts[1].messages.back().content,
               ContainsSubstring("# New events\n(Hulda is now following Aela.)\n(The door creaks.)\n"));
    const inference::Prompt& last = prompts[2];
    REQUIRE(last.stableCount == 5);
    CHECK(last.messages[1].content == "Aela: Hulda, follow me.");
    CHECK(last.messages[2].content == "Hulda [follow]: Lead the way, then.");
    CHECK(last.messages[3].content == "(Hulda is now following Aela.)\n(The door creaks.)\nAela: To the gate.");
    CHECK(last.messages[4].content == "Hulda [none]: Right behind you.");
    CHECK_THAT(last.messages.back().content, !ContainsSubstring("# New events"));
}

TEST_CASE("A join or a leave changes the prompt only from the participants layer on", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, NONE, NONE};
    script.lines = {"Lead the way, then.", "Welcome, Saadia.", "Just us now."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_join", backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);

    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), {}, {}));
    const inference::Prompt before = session.LastAssembly().prompt;
    session.SetParticipants(id, {test::SAADIA}, {}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Saadia, join us."), {}, {}));
    const inference::Prompt joined = session.LastAssembly().prompt;

    // Saadia joins at the end of the participants layer, which ends the system message here (no summary yet): the
    // first turn's system message is the start of the second's.
    const std::string& a = before.messages[0].content;
    const std::string& b = joined.messages[0].content;
    CHECK(b.starts_with(a));
    CHECK(b.substr(a.size()) == "- Saadia (0001B07C): Saadia serves at the inn.\n");
    CHECK(joined.messages[1].content == "Aela: Hulda, follow me.");

    // A leave changes the system message from the leaver's line on: L0, L1 and the participants before it stay.
    session.SetParticipants(id, {}, {test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, ""), {}, {}));
    const std::string c = session.LastAssembly().prompt.messages[0].content;
    const std::size_t aelaLine = b.find("- Aela (00000007)\n");
    REQUIRE(aelaLine != std::string::npos);
    CHECK(c.substr(0, aelaLine) == b.substr(0, aelaLine));
    CHECK(c.substr(aelaLine) == "- Saadia (0001B07C): Saadia serves at the inn.\n");
}

TEST_CASE("A cancel during the resolve or the speak call commits nothing", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, FOLLOW, FOLLOW};
    script.lines = {"Lead the way.", "Lead the way."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_cancel", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Follow me.");
    request.events = {R"({"text": "Rain starts."})"_json};

    // Cancelled before the resolve call writes anything.
    std::stop_source early;
    early.request_stop();
    CHECK_FALSE(session.RunTurn(request, {}, early.get_token()));

    // Cancelled while the line streams: onAction asks for the stop, so the speak call ends at its first piece.
    std::stop_source late;
    bool acted = false;
    turn::HostedCallbacks callbacks;
    callbacks.onAction = [&](const core::Adjudication&) {
        acted = true;
        late.request_stop();
    };
    CHECK_FALSE(session.RunTurn(request, callbacks, late.get_token()));
    CHECK(acted);
    CHECK(session.Turns(id).empty());
    CHECK(session.Events(id).empty()); // The request's events were not kept either.

    // The same request then plays as turn 1, with its events.
    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);
    CHECK(record->number == 1);
    CHECK(record->events == request.events);
}

TEST_CASE("Each malformed request is a DataError naming its field, before any model call", "[hosted][turn]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_bad_requests", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    const std::string ended = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.EndConversation(ended);
    // RunTurn of `request` is a DataError whose message holds `field`.
    auto fails = [&](const turn::HostedTurnRequest& request, const std::string& field) {
        INFO(field);
        CHECK_THROWS_MATCHES(session.RunTurn(request, {}, {}), core::DataError, MessageMatches(ContainsSubstring(field)));
    };

    fails(test::Request("c9", test::HULDA, "Hi."), "conversation_id: there is no conversation 'c9'");
    fails(test::Request(ended, test::HULDA, "Hi."), "conversation_id: conversation 'c2' has ended");
    turn::HostedTurnRequest badObservation = test::Request(id, test::HULDA, "Hi.");
    badObservation.observation.at("actors").at(0)["id"] = "";
    fails(badObservation, "observation at /actors/0/id");
    fails(test::Request(id, test::SAADIA, "Hi."), "speaker: '0001B07C' is not a participant of conversation 'c1'");
    fails(test::Request(id, test::AELA, "Hi."), "speaker: '00000007' is the player");
    turn::HostedTurnRequest absent = test::Request(id, test::HULDA, "Hi.");
    nlohmann::json& actors = absent.observation.at("actors");
    actors.erase(actors.begin()); // Hulda is gone; Saadia and Aela stay.
    fails(absent, "speaker: '0001A6A4' is not an actor of the observation");
    turn::HostedTurnRequest unknownAction = test::Request(id, test::HULDA, "Hi.");
    unknownAction.allowedActions = std::vector<std::string>{"follow", "fly"};
    fails(unknownAction, "allowed_actions/1: 'fly' is not an action of the catalog");
    turn::HostedTurnRequest badEvent = test::Request(id, test::HULDA, "Hi.");
    badEvent.events = {R"({"type": "ambient"})"_json};
    fails(badEvent, "events/0/text: an event needs 'text', a string");

    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("A player text with no room in the tail allowance is a TurnError", "[hosted][turn]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_long_text", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    const turn::HostedTurnRequest request = test::Request(id, test::HULDA, std::string(5000, 'x'));
    CHECK_THROWS_AS(session.RunTurn(request, {}, {}), turn::TurnError);
    CHECK_THROWS_WITH(session.RunTurn(request, {}, {}),
                      ContainsSubstring("the observation, the events and the player text use"));
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("allowedActions narrows the grammar the resolve call runs under and the schema it is checked against",
          "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"attack","params":{"target":"0001B07C"}})", NONE};
    script.lines = {"Leave, now.", "Fine."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_allowed", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Throw her out.");
    request.allowedActions = std::vector<std::string>{"follow", "attack"};

    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);
    CHECK(record->adjudication.requested);
    CHECK(record->adjudication.params == R"({"target": "0001B07C"})"_json);
    const std::string& gbnf = backend.Calls().at(0).spec.grammar;
    CHECK(grammar::Check(gbnf, FOLLOW).empty());
    CHECK(grammar::Check(gbnf, R"({"action":"attack","params":{"target":"0001B07C"}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, NONE).empty());

    // An action the turn does not allow is refused even when the model writes it.
    const std::optional<core::HostedTurn> next = session.RunTurn(request, {}, {});
    REQUIRE(next);
    CHECK_FALSE(next->adjudication.requested);
    REQUIRE(next->adjudication.reasons.size() == 1);
    CHECK(next->adjudication.reasons[0].starts_with("/action: unknown action 'none'"));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `'HostedTurnRequest': is not a member of 'morpheus::turn'` and `'RunTurn': is not a member of 'morpheus::turn::HostedSession'`.

- [ ] **Step 3: Make the call telemetry public**

In `src/turn/Session.h`, after the line `inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);`, insert:

```cpp

// One model call's numbers for a turn's telemetry (core::TurnRecord::timings, core::HostedTurn::timings). A call that
// was allowed to think also gets its thinking block: its numbers and its text, kept so the turn log can show why the
// model did what it did. The thinking text goes nowhere else: not into the narration, the transcript or a later
// prompt.
nlohmann::json CallTelemetry(const inference::Completion& completion, const inference::GenerationSpec& spec);
```

In `src/turn/Session.cpp`, delete the anonymous-namespace function `Telemetry` (its comment `// One model call's numbers for the turn telemetry (TurnRecord::timings).` with the two lines after it, the function, and the blank line after it). After the closing brace of `SamplingFrom`, insert:

```cpp

nlohmann::json CallTelemetry(const inference::Completion& completion, const inference::GenerationSpec& spec)
{
    nlohmann::json telemetry = {{"prompt_tokens", completion.promptTokens},  {"reused", completion.tokensReused},
                                {"evaluated", completion.tokensEvaluated},   {"generated", completion.tokensGenerated},
                                {"prompt_ms", completion.promptMs},          {"generate_ms", completion.generateMs},
                                {"stopped_at_limit", completion.stoppedAtLimit}};
    if (spec.thinkingMaxTokens > 0) {
        telemetry["thinking"] = {{"tokens", completion.thinkingTokens},
                                 {"ms", completion.thinkingMs},
                                 {"stopped_at_limit", completion.thinkingStoppedAtLimit},
                                 {"text", completion.thinking}};
    }
    return telemetry;
}
```

Then replace each of the five calls `Telemetry(` in `Session::PlayTurn` with `CallTelemetry(`: `Telemetry(looked, lookupSpec)`, `Telemetry(narrated, narrateSpec)`, `Telemetry(reviewed, spec)`, `Telemetry(renarrated, narrateSpec)` and `Telemetry(resolved, resolveSpec)`. Check: `grep -n "[^l]Telemetry(" src/turn/Session.cpp` prints nothing.

- [ ] **Step 4: Write RunTurn**

In `src/turn/HostedSession.h`, replace

```cpp
#include <cstdint>
#include <filesystem>
#include <optional>
```

with

```cpp
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
```

replace `#include "turn/HostedWorld.h"` with

```cpp
#include "turn/HostedWorld.h"
#include "turn/MemoryUpkeep.h"
```

after the line `constexpr std::string_view HOSTED_WORLD_ID = "hosted";`, insert:

```cpp

// One hosted turn as the host asks for it (hosted-world spec, section 1).
struct HostedTurnRequest {
    std::string conversationId = {};
    std::string speakerId = {};
    std::optional<std::string> playerText = {}; // Absent for a turn between NPCs.
    std::vector<nlohmann::json> events = {};    // Objects with a `text` string, added to the conversation before the turn.
    nlohmann::json observation = nlohmann::json::object();
    std::optional<std::vector<std::string>> allowedActions = {}; // Action ids; absent means every action of the catalog.
};

// What a hosted turn reports while it runs.
struct HostedCallbacks {
    // After the resolve call, before the line starts: what became of the speaker's proposed action.
    std::function<void(const core::Adjudication&)> onAction = {};
    // The line's pieces as they are written.
    inference::TokenCallback onToken = {};
};
```

after the `SetParticipants` declaration (its line `                         const std::vector<std::string>& left, const nlohmann::json& observation);`), insert:

```cpp

    // Plays one turn (hosted-world spec, section 2): checks the request, retrieves from lore and the run's folded
    // turns, resolves under the proposal grammar (the fit loop folding the conversation's oldest turns when L3 is
    // over its budget), adjudicates and reports the action through onAction, speaks on a prompt that extends the
    // resolve prompt (streamed through onToken), and commits the record. Player text is cleaned as Session cleans
    // input; nothing left means none. Returns the committed turn, or nullopt when `stop` cancelled it: then no turn is
    // committed and the request's events are not kept (folds the fit loop finished before the stop are, as in
    // Session). Throws DataError naming the field for a malformed request (before any model call), TurnError when
    // the tail does not fit tail_allowance, and InferenceError from the backend. Model mistakes are not exceptions: a
    // refused action is recorded with its reasons and the turn goes on with no action.
    std::optional<core::HostedTurn> RunTurn(const HostedTurnRequest& request, const HostedCallbacks& callbacks,
                                            std::stop_token stop);
```

and after the `SyncHistory` declaration (its line `    void SyncHistory();`), insert:

```cpp

    // One pass of the memory upkeep for a turn of conversation `conversationId`: the hosted prompt for `hits`, with
    // the tail parts `inputs` already holds (observation, events, player line, instruction) and the conversation, the
    // summaries and its unfolded entries read from the store as it is now; and the entries its L3 shows.
    FitPass Pass(std::string_view conversationId, const std::vector<RetrievalHit>& hits, context::HostedInputs inputs);

    // The shared memory upkeep for turn `turn` of conversation `conversationId`: a fold's summary joins the store with
    // the conversation's folded mark, and the folded turns become searchable history.
    MemoryUpkeep Upkeep(const std::string& conversationId, int turn);
```

In `src/turn/HostedSession.cpp`, replace

```cpp
#include <algorithm>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
```

with

```cpp
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
#include "turn/Session.h"
```

replace

```cpp
// The names of the host's lore in errors.
constexpr std::string_view HOST_LORE_SOURCE = "host data: lore";
```

with

```cpp
// The names of the host's lore in errors.
constexpr std::string_view HOST_LORE_SOURCE = "host data: lore";
// What a hosted tail holds, for the TurnError when it does not fit tail_allowance.
constexpr std::string_view HOSTED_TAIL_NAME = "the observation, the events and the player text";
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
constexpr std::array<store::EmbeddingKind, 2> HOSTED_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn};
```

After the definition of `HostedSession::SetParticipants` (before `void HostedSession::EndConversation`), insert:

```cpp
std::optional<core::HostedTurn> HostedSession::RunTurn(const HostedTurnRequest& request,
                                                       const HostedCallbacks& callbacks, std::stop_token stop)
{
    // 0. Check the request: nothing is stored and no model call is made before it passes. The conversation exists and
    // is open, the observation passes Observe, the speaker takes part, is present and is not the player, every
    // allowed action is in the catalog with one left after the actor scopes, and every event has its text.
    const core::Conversation conversation = OpenConversation(request.conversationId);
    const Observation observation = _world.Observe(request.observation);
    const bool participates = std::any_of(conversation.participants.begin(), conversation.participants.end(),
                                          [&](const core::Participant& participant) { return participant.id == request.speakerId; });
    if (!participates) {
        throw core::DataError(std::format("speaker: '{}' is not a participant of conversation '{}'", request.speakerId,
                                          conversation.id));
    }
    const Actor* pSpeaker = observation.Find(request.speakerId);
    if (pSpeaker == nullptr) {
        throw core::DataError(std::format("speaker: '{}' is not an actor of the observation", request.speakerId));
    }
    if (pSpeaker->isPlayer) {
        throw core::DataError(
            std::format("speaker: '{}' is the player; the host speaks for the player", request.speakerId));
    }
    const std::vector<std::string> allowed = request.allowedActions.value_or(_world.ActionIds());
    const nlohmann::json proposalSchema =
        _world.ProposalSchema(observation, conversation.participants, allowed, request.speakerId);
    HostedWorld::CheckEvents(request.events);

    // Player text is cleaned as Session cleans input: control characters other than newline and tab go (they have no
    // place in a prompt), and it is trimmed. Nothing left means no player text.
    std::optional<std::string> playerText;
    if (request.playerText) {
        std::string kept;
        for (const char c : *request.playerText) {
            const auto byte = static_cast<unsigned char>(c);
            if ((byte >= 0x20 && byte != 0x7F) || c == '\n' || c == '\t') {
                kept += c;
            }
        }
        std::string cleaned = core::Trim(kept);
        if (!cleaned.empty()) {
            playerText = std::move(cleaned);
        }
    }

    // The instructions name the speaker and the player. The speak instruction must fit narrate_allowance, which the
    // prompt reserves for it; the names come from the host.
    const Actor& player = observation.Player();
    const std::string resolveInstruction = FillVariables(HOSTED_RESOLVE_INSTRUCTION, pSpeaker->name, player.name);
    const std::string speakMessage = FillVariables(HOSTED_SPEAK_INSTRUCTION, pSpeaker->name, player.name);
    const int speakTokens = _pBackend->CountTokens(speakMessage);
    if (speakTokens > _settings.budget.narrateAllowance) {
        throw core::DataError(std::format("speaker: the speak instruction for '{}' uses {} tokens, over "
                                          "narrate_allowance {}",
                                          pSpeaker->name, speakTokens, _settings.budget.narrateAllowance));
    }

    const int turn = _store.LatestHostedTurn() + 1; // Run-wide, shared with the summaries' ranges.
    const std::uint64_t seed = _store.Meta().rngSeed;
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    const int lastTurn = played.empty() ? 0 : played.back().number;

    // The new events: those recorded since the conversation's last turn, then the request's, in order.
    std::vector<std::string> newEvents;
    for (const core::ConversationEvent& recorded : _store.ConversationEvents(conversation.id)) {
        if (recorded.afterTurn == lastTurn) {
            newEvents.push_back(recorded.event.at("text").get<std::string>());
        }
    }
    for (const nlohmann::json& event : request.events) {
        newEvents.push_back(event.at("text").get<std::string>());
    }

    // 1. Retrieve from lore and the run's folded turns (this and earlier conversations), with the player text, the
    // speaker's name and the previous line.
    RetrievalQuery query;
    query.input = playerText.value_or("");
    query.scene = {pSpeaker->name};
    query.digest = played.empty() ? std::string() : played.back().line;
    std::vector<RetrievalHit> hits =
        _index.Retrieve(query, HOSTED_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);

    // 2. Resolve: a prompt that fits (the fit loop sheds hits, condenses and folds), under the proposal grammar.
    context::HostedInputs tail;
    tail.observation = observation.json;
    tail.events = newEvents;
    tail.playerLine = playerText ? player.name + ": " + *playerText : std::string();
    tail.instruction = resolveInstruction;
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, turn).Fit(
        hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(conversation.id, kept, tail); }, stop);
    if (!assembly) {
        return std::nullopt;
    }
    _lastAssembly = *assembly;
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = _world.ProposalGrammar(observation, conversation.participants, allowed, request.speakerId);
    resolveSpec.maxTokens = _settings.budget.resolveMaxTokens;
    resolveSpec.thinkingMaxTokens = _settings.budget.resolveThinkingTokens;
    resolveSpec.sampling =
        SamplingFrom(_settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Resolve, 0));
    const inference::Completion resolved = _pBackend->Complete(assembly->prompt, resolveSpec, {}, stop);
    if (resolved.cancelled) {
        return std::nullopt;
    }
    // A refused action is the model's mistake: its reasons are kept, and the speaker takes no action.
    const core::Adjudication adjudication = _world.Adjudicate(proposalSchema, resolved.text);
    if (callbacks.onAction) {
        callbacks.onAction(adjudication);
    }

    // 3. Speak: the resolve prompt, the resolve output as the model wrote it, and the speak instruction, so nearly all
    // of the prompt stays cached. No grammar; the line streams.
    inference::Prompt speakPrompt = assembly->prompt;
    context::ExtendForNarrate(speakPrompt, resolved.text, speakMessage);
    inference::GenerationSpec speakSpec;
    speakSpec.maxTokens = _settings.budget.narrateMaxTokens;
    speakSpec.thinkingMaxTokens = _settings.budget.narrateThinkingTokens;
    speakSpec.sampling =
        SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, core::CallKind::Narrate, 0));
    const inference::Completion spoken = _pBackend->Complete(speakPrompt, speakSpec, callbacks.onToken, stop);
    if (spoken.cancelled) {
        return std::nullopt;
    }

    // 4. Commit the record: the listeners are the other participants present.
    core::HostedTurn record;
    record.number = turn;
    record.conversationId = conversation.id;
    record.speakerId = request.speakerId;
    for (const core::Participant& participant : conversation.participants) {
        if (participant.id != request.speakerId && observation.Find(participant.id) != nullptr) {
            record.listenerIds.push_back(participant.id);
        }
    }
    record.playerText = playerText;
    record.events = request.events;
    record.observation = observation.json;
    record.resolveOutput = resolved.text;
    record.adjudication = adjudication;
    record.line = core::Trim(spoken.text);
    nlohmann::json timings = nlohmann::json::object();
    timings["resolve"] = CallTelemetry(resolved, resolveSpec);
    if (!adjudication.requested) {
        timings["resolve"]["refused"] = adjudication.reasons; // The log of why the speaker took no action.
    }
    timings["speak"] = CallTelemetry(spoken, speakSpec);
    timings["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt, and why they matched.
    for (const RetrievalHit& hit : hits) {
        timings["retrieval"].push_back({{"source", hit.kind == store::EmbeddingKind::Lore ? "lore" : "history"},
                                        {"ref", hit.refId},
                                        {"score", hit.score},
                                        {"reasons", hit.reasons}});
    }
    record.timings = std::move(timings);
    _store.CommitHostedTurn(record);
    return record;
}

```

After the definition of `HostedSession::SyncHistory` (at the end, before `} // namespace morpheus::turn`), insert:

```cpp

FitPass HostedSession::Pass(std::string_view conversationId, const std::vector<RetrievalHit>& hits,
                            context::HostedInputs inputs)
{
    // The conversation as the store holds it now: a fold since the last pass moved its folded mark.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Pass: the conversation is not in the store");
    inputs.systemText = std::string(HOSTED_SYSTEM_TEXT);
    inputs.lore = _world.Lore();
    inputs.participants = conversation->participants;
    inputs.summaries = _store.Summaries();
    inputs.transcript = Entries(*conversation, false);
    inputs.retrieved.clear();
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }

    FitPass pass;
    pass.assembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
    pass.transcript = std::move(inputs.transcript);
    return pass;
}

MemoryUpkeep HostedSession::Upkeep(const std::string& conversationId, int turn)
{
    // A fold's summary joins the store with the conversation's folded mark, and the folded turns become history.
    return MemoryUpkeep(_store, *_pBackend, _settings, std::string(HOSTED_TAIL_NAME), turn, 0,
                        [this, conversationId](const core::Summary& summary) {
                            _store.FoldConversation(conversationId, summary);
                            SyncHistory();
                        });
}
```

`FillVariables` now has its second caller (`Entries` was the first); `Pass` and `Upkeep` get their second caller, `Maintain`, in Task 8.

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted],[session]"`
Expected: builds with no warnings; `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 482` (473 + 9), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Run the AddressSanitizer preset on the hosted tests**

Run: `cmake --preset asan && cmake --build --preset asan && ./build/asan/bin/Debug/morpheus_tests.exe "[hosted],[upkeep]"`
Expected: `All tests passed`, no AddressSanitizer report (the upkeep holds the session's members by pointer only while a turn runs).

- [ ] **Step 7: Byte check and commit**

```bash
FILES="src/turn/Session.h src/turn/Session.cpp src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedTestSupport.h tests/hosted/HostedTurnTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "HostedSession::RunTurn: check, retrieve, resolve and adjudicate, speak streamed, commit; CallTelemetry public" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

