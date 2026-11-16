### Task 3: The server, the fixtures and the docs

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp` (the turn route)
- Create: `tests/hosted/fixtures/decide.json`, `tests/server/ApiDecideTests.cpp`
- Modify tests: `tests/hosted/HostedFixtureTests.cpp` (`PlayFixture`), `tests/server/HttpServerTests.cpp` (`ReplayOverHttp`), `tests/server/ServerLiveTests.cpp`, `tests/server/ApiTurnTests.cpp` (the `"decide"` 400 check), every `"1.3.0"` pin under `tests/server/`
- Docs: `README.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`, `tests/hosted/fixtures/README.md`

**Interfaces:**
- Consumes: `HostedCallbacks::onSpeaker` (Task 2); `test::HostedScript::decisions`, `IsHostedDecide` (Task 2).
- Produces: a conversation fixture step's optional `model.decide` (the scripted decide answer) and `expect.speaker`.

- [ ] **Step 1: Write the failing tests**

Create `tests/hosted/fixtures/decide.json`:

```json
{
  "description": "R8's 'decide': Hulda and Saadia with Aela, the player. With player text or events both NPCs may answer (a decide call); with no input the last speaker is left out, so one candidate remains (no call).",
  "participants": ["0001A6A4", "0001B07C", "00000007"],
  "steps": [
    {
      "turn": {"speaker": "decide", "player_text": "Good evening, both of you."},
      "model": {"decide": "{\"speaker\":\"0001B07C\"}", "resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Evening, traveller."},
      "expect": {"speaker": "0001B07C", "requested": true, "line": "Evening, traveller.", "listeners": ["0001A6A4", "00000007"]}
    },
    {
      "turn": {"speaker": "decide", "player_text": "And you, Hulda?"},
      "model": {"decide": "{\"speaker\":\"0001A6A4\"}", "resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Business is slow."},
      "expect": {"speaker": "0001A6A4", "requested": true, "line": "Business is slow.", "listeners": ["0001B07C", "00000007"]}
    },
    {
      "turn": {"speaker": "decide"},
      "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "It will pick up."},
      "expect": {"speaker": "0001B07C", "requested": true, "line": "It will pick up.", "listeners": ["0001A6A4", "00000007"]}
    },
    {
      "turn": {"speaker": "decide", "events": [{"type": "ambient", "text": "A bard starts to sing."}]},
      "model": {"decide": "{\"speaker\":\"0001B07C\"}", "resolve": "{\"action\":\"none\",\"params\":{}}", "line": "I love this song."},
      "expect": {"speaker": "0001B07C", "requested": true, "line": "I love this song.", "listeners": ["0001A6A4", "00000007"]}
    }
  ]
}
```

`tests/hosted/HostedFixtureTests.cpp`, `PlayFixture` becomes `void PlayFixture(const std::string& name, bool decideEveryTurn = false)`:
- building the script, a step's `model.decide` (when present) is pushed to `script.decisions`;
- with `decideEveryTurn`, each turn's request goes with `request.speakerId = "decide"` (after `RequestFrom`), its expected speaker is the step's original `turn.speaker`, and the script's `decisions` is exactly `{R"({"speaker":")" + <the first turn step's speaker> + R"("})"}` (only the first turn has two candidates: no turn before it);
- after the record checks: `CHECK(record->speakerId == expectedSpeaker)`, where `expectedSpeaker` is `expect.speaker` when present, else (in `decideEveryTurn` mode) the original `turn.speaker`, else `turn.speaker`;
- at the end, with `decideEveryTurn`: `CHECK(script.decided == 1)`.

Add the cases:

```cpp
TEST_CASE("The decide fixture: the engine chooses the speaker, with a decide call only when two may answer",
          "[hosted][fixtures][decide]")
{
    PlayFixture("decide.json");
}

TEST_CASE("The radiant fixture with 'decide' on every turn alternates, with one decide call", "[hosted][fixtures][decide]")
{
    PlayFixture("radiant.json", true);
}
```

`tests/server/HttpServerTests.cpp`, `ReplayOverHttp`: before queueing the resolve completion, when `step.at("model").contains("decide")`, queue `{test::Pieces(step.at("model").at("decide").get<std::string>())}` first; after the `action` checks, when `expect` has `speaker`, `CHECK(action.at("speaker") == expect.at("speaker"));`. Add:

```cpp
TEST_CASE("The decide fixture plays over HTTP", "[hosted-server][http][fixtures][decide]")
{
    ReplayOverHttp("decide.json");
}
```

Create `tests/server/ApiDecideTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;

TEST_CASE("A decide turn's action event names the speaker the engine chose", "[hosted-server][api][decide]")
{
    test::HostedScript script;
    script.decisions = {R"({"speaker":"0001B07C"})"};
    script.actions = {R"({"action":"none","params":{}})"};
    script.lines = {"Evening."};
    test::ApiHarness harness("api_decide", script.Responder());
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::SAADIA, test::AELA}},
                          {"observation", test::FixtureObservation()}})
                .status == 200);
    const server::ApiResponse response =
        harness.Post("/v1/runs/save_1/conversations/c1/turn",
                     {{"speaker", "decide"}, {"player_text", "Evening, both."}, {"observation", test::FixtureObservation()}});
    REQUIRE(response.status == 200);
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().name == "action");
    CHECK(events.front().data.at("speaker") == test::SAADIA);
    CHECK(events.front().data.at("api_version") == "1.4.0");
    CHECK(events.back().name == "done");
}
```

`tests/server/ApiTurnTests.cpp`: delete the line `check(harness.Post(TURN, TurnBody("decide", "Hello.")), 400, "decide");`.

`tests/server/ServerLiveTests.cpp`, a new case after the conversation fixture case, on the same setup lines as that case (open with `open.json`, start the fixture's conversation):

```cpp
TEST_CASE("morpheus_server against real llama-servers: the decide fixture over HTTP chooses valid speakers",
          "[server][decide]")
{
    // [setup as in the conversation fixture case above, with runs folder "server_live_decide" and the fixture
    //  "decide.json"; copy those lines]
    std::string previous; // The last turn's speaker.
    for (const nlohmann::json& step : fixture.at("steps")) {
        nlohmann::json body = step.at("turn");
        body["observation"] = test::FixtureObservation();
        const test::HttpTurn turn = test::PostTurn(client, base + "/turn", body);
        REQUIRE(turn.status == 200);
        REQUIRE_FALSE(turn.events.empty());
        REQUIRE(turn.events.front().name == "action");
        const std::string speaker = turn.events.front().data.at("speaker").get<std::string>();
        // Only what cannot depend on the model's words: a candidate, and never the last speaker with no input.
        CHECK((speaker == test::HULDA || speaker == test::SAADIA));
        const bool noInput = !body.contains("player_text") && !body.contains("events");
        if (noInput && !previous.empty()) {
            CHECK(speaker != previous);
        }
        CHECK(turn.events.back().name == "done");
        previous = speaker;
    }
    runs.StopAll();
    http.Stop();
}
```

Write the setup lines out in full (they are the conversation case's lines from `const llamaserver::LlamaServerConfig config = ...` to `const std::string base = ...`); the bracketed comment above is only this plan's shorthand and must not appear in the file.

Change every `"1.3.0"` under `tests/server/` to `"1.4.0"`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[decide]"`
Expected: the server refuses `"decide"` with 400; the version pins fail.

- [ ] **Step 3: Implement**

`src/server/Api.h`: `inline constexpr std::string_view API_VERSION = "1.4.0";`

`src/server/Api.cpp`, the turn route: delete the `if (request.speakerId == "decide") { throw ... }` block. In the job, before `callbacks.onAction`:

```cpp
                                // The turn's speaker as the session reports it: the named one, or the NPC a "decide"
                                // turn chose.
                                std::string speakerId = request.speakerId;
                                callbacks.onSpeaker = [&speakerId](const std::string& id) { speakerId = id; };
```

and the `action` event's `{"speaker", request.speakerId}` becomes `{"speaker", speakerId}`.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all pass.

- [ ] **Step 5: Docs**

- `CHANGELOG.md`, above `## API 1.3.0`:
  ```markdown
  ## API 1.4.0

  The engine chooses the speaker (design: `docs/superpowers/specs/2026-11-16-decide-speaker-design.md`). Only additions.

  - A turn's `speaker` may be `"decide"`: the engine chooses one of the conversation's NPCs present (not the player).
    On a turn with no player text and no events the last speaker is left out while another remains. With one candidate
    it is chosen at once; with more, a short model call chooses. The `action` event's `speaker` names the chosen NPC.
    No NPC participant in the observation is a 400.
  - The prompt pack gains `decide` (variable `{player}`), the decide call's instruction; `GET /v1/prompts/default`
    lists it.
  - `decide` is reserved: an observation actor with that id is a 400.
  - A decided turn's `timings` gain `decide` (the call, and `refused` with the reasons when the model's answer was
    refused and the candidate who spoke least recently was chosen instead).
  ```
- `README.md`, the `morpheus_server` section: `api_version` is 1.4.0 (the sentence and the SSE example); the turn route says `speaker` is an NPC id or `"decide"`; a **Decide.** paragraph after the Prompt pack paragraph saying what the spec's sections 2-4 say for a host (who may be chosen, the no-input rule, one candidate means no call, the `action` event names the speaker, the refused-answer rule and `timings.decide`), linking the spec; the Prompt pack paragraph lists `decide` among the texts with its variable.
- `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`: the turn route's `speaker` accepts `"decide"` (added in 1.4.0, G1); the `api_version` line says 1.4.0; the Documentation section's CHANGELOG bullet gains "API 1.4.0 (G1, `2026-11-16-decide-speaker-design.md`) adds `speaker: "decide"` and the pack's `decide` text."
- `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`, section 1's table: a row for `decide` after `turn_cue` (the decide call's instruction, after the tail's `# Who may speak` list; variables `{player}`; added by G1, `2026-11-16-decide-speaker-design.md`).
- `tests/hosted/fixtures/README.md`, Conversation fixtures: the heading lists `decide.json`; `turn.speaker` may be `"decide"`; `model.decide` (optional) is the reply to the decide call, used in step order by steps whose turn makes one; `expect.speaker` (optional) is the committed turn's speaker id.

ASCII and control-byte greps on every changed file (both print 0).

- [ ] **Step 6: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp tests/hosted/fixtures/decide.json tests/server/ApiDecideTests.cpp tests/hosted/HostedFixtureTests.cpp tests/server/HttpServerTests.cpp tests/server/ServerLiveTests.cpp tests/server/ApiTurnTests.cpp tests/server/*.cpp README.md CHANGELOG.md docs/superpowers/specs/2026-11-16-morpheus-server-design.md docs/superpowers/specs/2026-11-16-prompt-pack-design.md tests/hosted/fixtures/README.md
git commit -m "Server: speaker 'decide', the chosen speaker in the action event, API 1.4.0; the decide fixture; docs"
```

---

## Finally (controller)

- `ctest --preset tests`; `cmake --build --preset asan` and `ctest --preset asan`.
- `"[turn]"`, `"[replay]"`, `"[context]"` pass (Morpheus's own game).
- `cmake --build build/dev --config RelWithDebInfo` builds clean; the consumer check is ok.
- Live: `[server]` over llama-server (the new decide case, and the others).
