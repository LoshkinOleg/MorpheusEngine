### Task 9: Conversation fixtures, the scripted-conversation tests and the model-backed conversation

Fixture files written from section 5 of the requirements drive whole conversations with the fake backend: a scripted player conversation (events between and with turns, an action with parameters, a refused action, a turn without player text), a radiant conversation of two NPCs over six turns, and the requests that must fail with their `DataError`. The host's recorded fixtures can replace them later in the same format. One `[llama]` test plays a short hosted conversation on the real model.

**Files:**
- Create: `tests/hosted/fixtures/conversation.json`, `tests/hosted/fixtures/radiant.json`, `tests/hosted/fixtures/invalid_requests.json`
- Create: `tests/hosted/HostedFixtureTests.cpp`
- Create: `tests/llama/LlamaHostedTests.cpp`

**Interfaces:**
- Consumes: everything of Tasks 4-8 through `turn::HostedSession` and `tests/hosted/HostedTestSupport.h`; `test::SharedBackend()` (`tests/llama/LlamaTestModels.h`).
- Produces: the fixture format. A conversation fixture is `{"participants": [ids], "steps": [...]}`; a step is either `{"events": [event objects]}` (recorded between turns) or `{"turn": {...}, "model": {"resolve": text, "line": text}, "expect": {...}}`. A `turn` holds `speaker` and optionally `player_text`, `events`, `allowed_actions` and `observation` (default: `observation.json`). `expect` holds `requested`, `line`, `listeners`, and optionally `action` (`{"id", "params"}`, the turn result of section 5.3), `reasons`, `l3` (the contents of the turn's L3 messages, in order) and `new_events` (the texts the tail lists). The invalid-request fixture is `{"participants": [ids], "requests": [{"name", "conversation": "open" | "ended" | an id, "turn": {...}, "error": substring}]}`.

- [ ] **Step 1: Write the fixtures**

Create `tests/hosted/fixtures/conversation.json`:

```json
{
  "description": "Aela asks Hulda to follow; an action result arrives between turns and an ambient event with a turn; Hulda offers an attack with a target, a wait with an extra key is refused, and Hulda speaks on without player text. Written from section 5 of the integration requirements.",
  "participants": ["0001A6A4", "00000007"],
  "steps": [
    {
      "turn": {"speaker": "0001A6A4", "player_text": "Hulda, I need you to follow me."},
      "model": {"resolve": "{\"action\":\"follow\",\"params\":{}}", "line": "Lead the way, then."},
      "expect": {"requested": true, "action": {"id": "follow", "params": {}}, "line": "Lead the way, then.",
                 "listeners": ["00000007"], "l3": [], "new_events": []}
    },
    {
      "events": [{"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true,
                  "text": "Hulda is now following Aela."}]
    },
    {
      "turn": {"speaker": "0001A6A4", "player_text": "Saadia keeps staring at us.",
               "events": [{"type": "ambient", "text": "Saadia glances at the door."}]},
      "model": {"resolve": "{\"action\":\"attack\",\"params\":{\"target\":\"0001B07C\"}}",
                "line": "Say the word and I will deal with her."},
      "expect": {"requested": true, "action": {"id": "attack", "params": {"target": "0001B07C"}},
                 "line": "Say the word and I will deal with her.", "listeners": ["00000007"],
                 "l3": ["Aela: Hulda, I need you to follow me.", "Hulda [follow]: Lead the way, then."],
                 "new_events": ["Hulda is now following Aela.", "Saadia glances at the door."]}
    },
    {
      "turn": {"speaker": "0001A6A4", "player_text": "No. Wait here for me.",
               "allowed_actions": ["none", "wait", "end_conversation"]},
      "model": {"resolve": "{\"action\":\"wait\",\"params\":{},\"intent\":\"rest\"}", "line": "As you wish."},
      "expect": {"requested": false,
                 "reasons": ["/intent: unexpected key 'intent'; the keys are 'action', 'params'"],
                 "line": "As you wish.", "listeners": ["00000007"],
                 "l3": ["Aela: Hulda, I need you to follow me.", "Hulda [follow]: Lead the way, then.",
                        "(Hulda is now following Aela.)\n(Saadia glances at the door.)\nAela: Saadia keeps staring at us.",
                        "Hulda [attack {\"target\":\"0001B07C\"}]: Say the word and I will deal with her."],
                 "new_events": []}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"end_conversation\",\"params\":{}}", "line": "I will be here when you return."},
      "expect": {"requested": true, "action": {"id": "end_conversation", "params": {}},
                 "line": "I will be here when you return.", "listeners": ["00000007"],
                 "l3": ["Aela: Hulda, I need you to follow me.", "Hulda [follow]: Lead the way, then.",
                        "(Hulda is now following Aela.)\n(Saadia glances at the door.)\nAela: Saadia keeps staring at us.",
                        "Hulda [attack {\"target\":\"0001B07C\"}]: Say the word and I will deal with her.",
                        "Aela: No. Wait here for me.", "Hulda: As you wish."],
                 "new_events": []}
    }
  ]
}
```

Create `tests/hosted/fixtures/radiant.json`:

```json
{
  "description": "Hulda and Saadia talk for six turns with no player input (requirement R8's acceptance), alternating, each with a valid action.",
  "participants": ["0001A6A4", "0001B07C"],
  "steps": [
    {
      "turn": {"speaker": "0001B07C"},
      "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Another quiet night."},
      "expect": {"requested": true, "action": {"id": "none", "params": {}}, "line": "Another quiet night.",
                 "listeners": ["0001A6A4"], "l3": []}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Quiet suits me fine."},
      "expect": {"requested": true, "line": "Quiet suits me fine.", "listeners": ["0001B07C"]}
    },
    {
      "turn": {"speaker": "0001B07C"},
      "model": {"resolve": "{\"action\":\"give_item\",\"params\":{\"item\":\"Sweetroll\"}}", "line": "Here, have a sweetroll."},
      "expect": {"requested": true, "action": {"id": "give_item", "params": {"item": "Sweetroll"}},
                 "line": "Here, have a sweetroll.", "listeners": ["0001A6A4"]}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"follow\",\"params\":{}}", "line": "Show me what you found."},
      "expect": {"requested": true, "line": "Show me what you found.", "listeners": ["0001B07C"]}
    },
    {
      "turn": {"speaker": "0001B07C"},
      "model": {"resolve": "{\"action\":\"wait\",\"params\":{}}", "line": "Wait, someone is coming."},
      "expect": {"requested": true, "line": "Wait, someone is coming.", "listeners": ["0001A6A4"]}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Then we wait together."},
      "expect": {"requested": true, "line": "Then we wait together.", "listeners": ["0001B07C"],
                 "l3": ["(Saadia speaks.)", "Saadia [none]: Another quiet night.",
                        "(Hulda speaks.)", "Hulda [none]: Quiet suits me fine.",
                        "(Saadia speaks.)", "Saadia [give_item {\"item\":\"Sweetroll\"}]: Here, have a sweetroll.",
                        "(Hulda speaks.)", "Hulda [follow]: Show me what you found.",
                        "(Saadia speaks.)", "Saadia [wait]: Wait, someone is coming."],
                 "new_events": []}
    }
  ]
}
```

Create `tests/hosted/fixtures/invalid_requests.json`:

```json
{
  "description": "Requests a host may send that Morpheus must refuse with a DataError naming the field, before any model call.",
  "participants": ["0001A6A4", "00000007"],
  "requests": [
    {"name": "unknown conversation", "conversation": "c9",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello."},
     "error": "conversation_id: there is no conversation 'c9'"},
    {"name": "ended conversation", "conversation": "ended",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello."},
     "error": "conversation_id: conversation 'c2' has ended"},
    {"name": "speaker who takes no part", "conversation": "open",
     "turn": {"speaker": "0001B07C", "player_text": "Hello."},
     "error": "speaker: '0001B07C' is not a participant of conversation 'c1'"},
    {"name": "the player as speaker", "conversation": "open",
     "turn": {"speaker": "00000007", "player_text": "Hello."},
     "error": "speaker: '00000007' is the player"},
    {"name": "speaker not present", "conversation": "open",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello.",
              "observation": {"game_time": {"days_passed": 1, "hour": 9}, "location": "Whiterun",
                              "actors": [{"id": "00000007", "name": "Aela", "is_player": true}]}},
     "error": "speaker: '0001A6A4' is not an actor of the observation"},
    {"name": "unknown allowed action", "conversation": "open",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello.", "allowed_actions": ["follow", "fly"]},
     "error": "allowed_actions/1: 'fly' is not an action of the catalog"},
    {"name": "event without text", "conversation": "open",
     "turn": {"speaker": "0001A6A4", "events": [{"type": "ambient"}]},
     "error": "events/0/text: an event needs 'text', a string"},
    {"name": "actor without an id", "conversation": "open",
     "turn": {"speaker": "0001A6A4",
              "observation": {"game_time": {"days_passed": 1, "hour": 9}, "location": "Whiterun",
                              "actors": [{"id": "", "name": "Hulda", "is_player": false},
                                         {"id": "00000007", "name": "Aela", "is_player": true}]}},
     "error": "observation at /actors/0/id"},
    {"name": "observation the host's schema refuses", "conversation": "open",
     "turn": {"speaker": "0001A6A4",
              "observation": {"game_time": {"days_passed": 1, "hour": 25}, "location": "Whiterun",
                              "actors": [{"id": "0001A6A4", "name": "Hulda", "is_player": false},
                                         {"id": "00000007", "name": "Aela", "is_player": true}]}},
     "error": "observation at /game_time/hour: expected an integer from 0 to 23, got 25"}
  ]
}
```

- [ ] **Step 2: Write the fixture tests and the model-backed test**

Create `tests/hosted/HostedFixtureTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
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
using Catch::Matchers::MessageMatches;

namespace {

// The request a fixture's "turn" describes, in conversation `conversationId`. Without an "observation" it carries
// observation.json.
turn::HostedTurnRequest RequestFrom(const nlohmann::json& turnJson, const std::string& conversationId)
{
    turn::HostedTurnRequest request;
    request.conversationId = conversationId;
    request.speakerId = turnJson.at("speaker").get<std::string>();
    if (turnJson.contains("player_text")) {
        request.playerText = turnJson.at("player_text").get<std::string>();
    }
    if (turnJson.contains("events")) {
        request.events = turnJson.at("events").get<std::vector<nlohmann::json>>();
    }
    request.observation = turnJson.contains("observation") ? turnJson.at("observation") : test::FixtureObservation();
    if (turnJson.contains("allowed_actions")) {
        request.allowedActions = turnJson.at("allowed_actions").get<std::vector<std::string>>();
    }
    return request;
}

// Plays the conversation fixture `name` with the fake backend replying as its steps say, and checks each turn
// against its "expect": the record, the L3 its prompt showed and the new events its tail listed.
void PlayFixture(const std::string& name)
{
    const nlohmann::json fixture = test::ReadFixture(name);
    const nlohmann::json& steps = fixture.at("steps");
    test::HostedScript script;
    for (const nlohmann::json& step : steps) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fixture_" + name),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation(fixture.at("participants").get<std::vector<std::string>>(),
                                                     test::FixtureObservation());

    int played = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const nlohmann::json& step = steps[i];
        INFO(name << " step " << i);
        if (step.contains("events")) {
            session.RecordEvents(id, step.at("events").get<std::vector<nlohmann::json>>());
            continue;
        }
        const std::optional<core::HostedTurn> record = session.RunTurn(RequestFrom(step.at("turn"), id), {}, {});
        REQUIRE(record);
        ++played;

        // The record.
        const nlohmann::json& expect = step.at("expect");
        CHECK(record->number == played);
        CHECK(record->adjudication.requested == expect.at("requested").get<bool>());
        if (expect.contains("action")) {
            CHECK(record->adjudication.actionId == expect.at("action").at("id").get<std::string>());
            CHECK(record->adjudication.params == expect.at("action").at("params"));
        }
        if (expect.contains("reasons")) {
            CHECK(record->adjudication.reasons == expect.at("reasons").get<std::vector<std::string>>());
        }
        CHECK(record->line == expect.at("line").get<std::string>());
        CHECK(record->listenerIds == expect.at("listeners").get<std::vector<std::string>>());

        // The turn's prompt: its L3 messages, and the new events in its tail.
        const inference::Prompt& prompt = session.LastAssembly().prompt;
        if (expect.contains("l3")) {
            std::vector<std::string> l3;
            for (std::size_t k = 1; k < prompt.stableCount; ++k) {
                l3.push_back(prompt.messages[k].content);
            }
            CHECK(l3 == expect.at("l3").get<std::vector<std::string>>());
        }
        if (expect.contains("new_events")) {
            const std::vector<std::string> events = expect.at("new_events").get<std::vector<std::string>>();
            const std::string& tail = prompt.messages.back().content;
            if (events.empty()) {
                CHECK_THAT(tail, !ContainsSubstring("# New events"));
            } else {
                std::string block = "# New events\n";
                for (const std::string& event : events) {
                    block += "(" + event + ")\n";
                }
                CHECK_THAT(tail, ContainsSubstring(block));
            }
        }
    }
    CHECK(session.Turns(id).size() == static_cast<std::size_t>(played));
}

} // namespace

TEST_CASE("The scripted conversation fixture plays with the records and the L3 it expects", "[hosted][fixtures]")
{
    PlayFixture("conversation.json");
}

TEST_CASE("The radiant fixture: two NPCs talk for six turns with no player text", "[hosted][fixtures]")
{
    PlayFixture("radiant.json");
}

TEST_CASE("Each invalid request fixture is a DataError naming its field, before any model call", "[hosted][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("invalid_requests.json");
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fixture_invalid"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::vector<std::string> participants = fixture.at("participants").get<std::vector<std::string>>();
    const std::string open = session.StartConversation(participants, test::FixtureObservation());
    const std::string ended = session.StartConversation(participants, test::FixtureObservation());
    session.EndConversation(ended);

    for (const nlohmann::json& entry : fixture.at("requests")) {
        INFO(entry.at("name").get<std::string>());
        const std::string named = entry.at("conversation").get<std::string>();
        const std::string conversationId = named == "open" ? open : named == "ended" ? ended : named;
        CHECK_THROWS_MATCHES(session.RunTurn(RequestFrom(entry.at("turn"), conversationId), {}, {}), core::DataError,
                             MessageMatches(ContainsSubstring(entry.at("error").get<std::string>())));
    }
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(open).empty());
}
```

Create `tests/llama/LlamaHostedTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "hosted/HostedTestSupport.h"
#include "llama/LlamaTestModels.h"
#include "turn/HostedSession.h"

using namespace morpheus;

TEST_CASE("A hosted conversation on the real model: every action validates and every line is spoken", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // Budgets for the shared backend's context of 4096, greedy sampling, no thinking.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.0, 40, 0.95};
    settings.narrateSampling = {0.0, 40, 0.95};
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("llama_hosted"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    const std::vector<std::string> playerTexts = {"Hulda, I need a room for the night.",
                                                  "Saadia keeps staring at us. Should I worry?",
                                                  "Then follow me, we are leaving."};
    for (const std::string& text : playerTexts) {
        INFO("player: " << text);
        std::string streamed;
        turn::HostedCallbacks callbacks;
        callbacks.onToken = [&streamed](std::string_view piece) { streamed += piece; };
        const std::optional<core::HostedTurn> record = session.RunTurn(test::Request(id, test::HULDA, text), callbacks, {});
        REQUIRE(record);
        INFO("resolve: " << record->resolveOutput << "; line: " << record->line);
        CHECK(record->adjudication.requested); // The grammar keeps the proposal valid.
        CHECK(record->adjudication.reasons.empty());
        CHECK_FALSE(record->line.empty());
        CHECK_FALSE(streamed.empty());
    }
    CHECK(session.Turns(id).size() == playerTexts.size());
}
```

- [ ] **Step 3: Run the fixture tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[fixtures]"`
Expected: builds with no warnings; `All tests passed` (3 test cases). These tests need no new engine code: they pin Tasks 4-8 against the fixtures. A failure here is a mismatch between a fixture and the engine's rendering; fix the engine, not the fixture, unless the fixture contradicts the spec.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 488` (485 + 3), the three `[server]` tests "Not Run (Skipped)". (`tests/llama` is not built in this preset.)

- [ ] **Step 4: Build the dev targets and run the model-backed test**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run, with the two model variables exported (Global Constraints): `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "A hosted conversation on the real model: every action validates and every line is spoken"`
Expected: `All tests passed` (1 test case). If the variables are unset the test is skipped: export them and run again.

- [ ] **Step 5: Byte check and commit**

```bash
FILES="tests/hosted/fixtures/conversation.json tests/hosted/fixtures/radiant.json tests/hosted/fixtures/invalid_requests.json tests/hosted/HostedFixtureTests.cpp tests/llama/LlamaHostedTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Hosted conversation fixtures from the host's data contracts, the scripted and radiant conversation tests, and a model-backed hosted conversation" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

