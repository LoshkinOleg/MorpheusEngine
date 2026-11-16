### Task 5: The server, the fixture and the docs

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp` (the defaults route)
- Modify: `tests/hosted/HostedTestSupport.h` (`PlayMemoryFixture`), `tests/server/ServerTestSupport.h` (`PlayMemoryFixtureOverHttp`)
- Create: `tests/hosted/fixtures/prompt_pack.json`, `tests/server/ApiPromptTests.cpp`
- Modify tests: `tests/hosted/HostedFixtureTests.cpp`, `tests/server/HttpMemoryTests.cpp`, `tests/llama/LlamaHostedTests.cpp`, every test pinning `"1.2.0"` (`tests/server/ApiMemoryTests.cpp`, `ApiRewindTests.cpp`, `ApiTests.cpp`, `ApiTurnTests.cpp`, `HttpServerTests.cpp`)
- Docs: `README.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, `tests/hosted/fixtures/README.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `PROMPT_TEXTS`, `PromptPack::Text` (Task 1), `HostData::prompts` (Task 2).
- Produces: `GET /v1/prompts/default` -> `{"prompts": {<key>: <text>}, "variables": {<key>: [names]}, "api_version": "1.3.0"}`; a memory fixture's optional `"prompts"`.

- [ ] **Step 1: Write the failing tests**

Create `tests/server/ApiPromptTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"
#include "turn/PromptPack.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The defaults route answers every default text and the variables it takes", "[hosted-server][api][prompts]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_prompts_default", script.Responder());
    const server::ApiResponse response = harness.Get("/v1/prompts/default");
    CHECK(response.status == 200);
    CHECK(response.body.at("api_version") == "1.3.0");
    const turn::PromptPack defaults;
    const nlohmann::json& prompts = response.body.at("prompts");
    CHECK(prompts.size() == 6);
    for (const turn::PromptText& text : turn::PROMPT_TEXTS) {
        INFO(text.key);
        CHECK(prompts.at(std::string(text.key)) == defaults.Text(text.key));
    }
    CHECK(response.body.at("variables") == nlohmann::json{{"system", nlohmann::json::array()},
                                                          {"resolve", {"speaker", "player"}},
                                                          {"speak", {"speaker", "player"}},
                                                          {"turn_cue", {"speaker", "player"}},
                                                          {"summarize", {"actor"}},
                                                          {"condense", {"actor"}}});
}

TEST_CASE("Open takes a pack: the next turn's prompt shows it; a bad pack is a 400 naming the text and the variable",
          "[hosted-server][api][prompts]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})"};
    script.lines = {"Welcome."};
    test::ApiHarness harness("api_prompts_open", script.Responder());

    nlohmann::json bad = test::ReadFixture("open.json");
    bad["prompts"] = {{"speak", "Say it to {location}."}};
    const server::ApiResponse refused = harness.Post("/v1/runs/save_1/open", bad);
    CHECK(refused.status == 400);
    CHECK_THAT(refused.body.at("error").at("message").get<std::string>(),
               ContainsSubstring("prompts/speak: unknown variable {location}; this text takes {speaker}, {player}"));

    nlohmann::json open = test::ReadFixture("open.json");
    open["prompts"] = {{"system", "# Pack\nYou voice a test scene."}, {"resolve", "PACK: {speaker} acts."}};
    REQUIRE(harness.Post("/v1/runs/save_1/open", open).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                {{"speaker", test::HULDA}, {"player_text", "Hi."}, {"observation", test::FixtureObservation()}})
                       .stream);
    // The run's backend is the last one made (the refused open may have made one before its host data failed).
    REQUIRE_FALSE(harness.backends.empty());
    const std::vector<test::ScriptedBackend::Call>& calls = harness.backends.back()->Calls();
    REQUIRE_FALSE(calls.empty());
    CHECK(calls[0].prompt.messages[0].content.starts_with("# Pack\nYou voice a test scene."));
    CHECK(calls[0].prompt.messages.back().content.ends_with("# Task\nPACK: Hulda acts."));
}
```

Create `tests/hosted/fixtures/prompt_pack.json`:

```json
{
  "description": "R7's acceptance: a host pack replaces the system text, the resolve instruction, the speak instruction and the turn cue; every resolve prompt shows the pack's texts and none of the replaced defaults. The fold and condense texts stay the engine's.",
  "prompts": {
    "system": "# Voices\nYou voice the people of a tavern scene that a host game runs. Step 1: reply with one JSON object choosing what the speaker tries; most turns need the action that does nothing. Step 2: write only the speaker's spoken words, one to three sentences, in character.",
    "resolve": "Step 1 for {speaker}: the JSON object only.",
    "speak": "Step 2: {speaker} speaks now. Spoken words only.",
    "turn_cue": "({speaker} goes on talking.)"
  },
  "observation": {
    "game_time": {"days_passed": 213.42, "hour": 14},
    "location": "Whiterun, The Bannered Mare",
    "actors": [
      {"id": "0001A6A4", "name": "Hulda", "is_player": false, "bio": "Owner of the Bannered Mare."},
      {"id": "0001B07C", "name": "Saadia", "is_player": false, "bio": "A Redguard working at the inn."},
      {"id": "00000007", "name": "Aela", "is_player": true}
    ]
  },
  "participants": ["0001A6A4", "0001B07C", "00000007"],
  "steps": [
    {"turn": {"speaker": "0001A6A4", "player_text": "Good evening."},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Welcome, traveller."},
     "expect": {"prompt_contains": ["# Voices\nYou voice the people of a tavern scene", "Step 1 for Hulda: the JSON object only."],
                "prompt_absent": ["You voice the characters of a role-playing game", "Reply with the JSON object only."]}},
    {"turn": {"speaker": "0001A6A4"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "The stew is fresh today."}},
    {"turn": {"speaker": "0001B07C", "player_text": "And you, Saadia?"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "I keep the rooms clean."},
     "expect": {"prompt_contains": ["(Hulda goes on talking.)", "Step 1 for Saadia: the JSON object only."],
                "prompt_absent": ["(Hulda speaks.)", "You voice the characters of a role-playing game"]}},
    {"end": true},
    {"maintain": true},
    {"memories": {"actor": "0001A6A4", "contains": ["stew is fresh"]}}
  ]
}
```

Add the cases:
- `tests/hosted/HostedFixtureTests.cpp`:
  ```cpp
  TEST_CASE("The prompt-pack fixture: every resolve prompt shows the host's texts", "[hosted][fixtures][prompts]")
  {
      test::PlayMemoryFixture("prompt_pack.json");
  }
  ```
- `tests/server/HttpMemoryTests.cpp`:
  ```cpp
  TEST_CASE("The prompt-pack fixture plays over HTTP", "[hosted-server][http][prompts][fixtures]")
  {
      const nlohmann::json fixture = test::ReadFixture("prompt_pack.json");
      test::HostedScript script = test::ScriptOf(fixture);
      test::ApiHarness harness("http_prompt_pack_fixture", test::MemoryEchoResponder(script));
      server::HttpServer http(harness.api);
      http.Start(0);
      httplib::Client client("127.0.0.1", http.Port());
      test::PlayMemoryFixtureOverHttp(client, "r", fixture, true);
      harness.runs.StopAll();
      http.Stop();
  }
  ```
- `tests/llama/LlamaHostedTests.cpp`, after the reload case:
  ```cpp
  TEST_CASE("The prompt-pack fixture on the real model: the host's texts replace the defaults", "[llama][hosted][prompts]")
  {
      llama::LlamaCppBackend& backend = test::SharedBackend();
      // As the tests above: budgets that fit the shared backend's context of 4096, greedy sampling.
      core::Settings settings = test::HostedSettings();
      settings.contextSize = backend.ContextSize();
      settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
      settings.resolveSampling = {0.0, 40, 0.95};
      settings.narrateSampling = {0.0, 40, 0.95};
      // The prompts' texts never depend on the model's words: every prompt_absent is checked.
      test::PlayMemoryFixture("prompt_pack.json", backend, settings, false);
  }
  ```

Change every `"1.2.0"` in `tests/server/*.cpp` to `"1.3.0"`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`
Expected: the defaults route answers 404; the fixture cases fail (`PlayMemoryFixture` ignores `prompts`, so the default texts show); the version checks fail.

- [ ] **Step 3: Implement**

`src/server/Api.h`: `inline constexpr std::string_view API_VERSION = "1.3.0";`

`src/server/Api.cpp`: include `"turn/PromptPack.h"`; after the `health` branch:

```cpp
        } else if (get && route({"v1", "prompts", "default"})) {
            // The engine's default texts and the variables each takes, for a host writing its own pack.
            const turn::PromptPack defaults;
            nlohmann::json prompts = nlohmann::json::object();
            nlohmann::json variables = nlohmann::json::object();
            for (const turn::PromptText& text : turn::PROMPT_TEXTS) {
                const std::string key(text.key);
                prompts[key] = defaults.Text(text.key);
                variables[key] = nlohmann::json::array();
                for (const std::string_view name : text.variables) {
                    if (!name.empty()) {
                        variables[key].push_back(std::string(name));
                    }
                }
            }
            response.body = {{"prompts", std::move(prompts)}, {"variables", std::move(variables)}};
```

`tests/hosted/HostedTestSupport.h`, `PlayMemoryFixture`, after the clock (add `#include <map>`):

```cpp
    // A fixture with "prompts" sends that pack with the host data.
    if (fixture.contains("prompts")) {
        data.prompts = fixture.at("prompts").get<std::map<std::string, std::string>>();
    }
```

`tests/server/ServerTestSupport.h`, `PlayMemoryFixtureOverHttp`, after the clock:

```cpp
    // A fixture with "prompts" sends that pack with the host data.
    if (fixture.contains("prompts")) {
        hostData["prompts"] = fixture.at("prompts");
    }
```

and update both functions' comments ("A fixture with a "clock" ..." gains "and with "prompts", the host data's pack").

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: the whole suite passes.

- [ ] **Step 5: Docs**

- `CHANGELOG.md`, above `## API 1.2.0`:
  ```markdown
  ## API 1.3.0

  The host's prompt pack (design: `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`). Only additions; two default texts changed.

  - The host data may carry `prompts`: any of `system`, `resolve`, `speak`, `turn_cue`, `summarize`, `condense`, each a
    text that replaces the engine's default for the run. Variables: `system` none; `resolve`, `speak`, `turn_cue`
    `{speaker}` and `{player}`; `summarize` and `condense` `{actor}`. An unknown key, an empty text, a variable the text
    does not take, or a system text over `engine_rules_cap` fails the `open` with 400 naming `prompts/<key>`.
  - `GET /v1/prompts/default` answers `{prompts, variables}`: the engine's default texts and the variables each takes.
  - The default speak instruction no longer names the player, and the default system text says most turns need no
    action.
  ```
- `README.md`, the `morpheus_server` route table: the `open` row's body becomes `{catalog, observation_schema, lore?, clock?, prompts?, seed?}`; add a row `| GET /v1/prompts/default | | {prompts, variables}: the default texts and their variables |`; after the Rewind paragraph add a **Prompt pack.** paragraph: what `prompts` holds (the six keys, what each text is, its variables), that a key left out keeps the engine's default, the `{name}` variable rule (other braces are literal), the errors at open, and that a changed pack is changed host data (the next prompt uses it). Link the spec.
- `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`: the `open` row gains `prompts?`; add the defaults route row ("added in 1.3.0 (F)"); the `api_version` line says 1.3.0; the CHANGELOG bullet in "Documentation" gains "API 1.3.0 (F, `2026-11-16-prompt-pack-design.md`) adds the host data's optional `prompts` and the defaults route."
- `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, "Default hosted texts": replace "Sub-project F makes them replaceable by the host." with "Sub-project F made them replaceable by the host (`2026-11-16-prompt-pack-design.md`)."
- `tests/hosted/fixtures/README.md`: the memory fixtures heading lists `prompt_pack.json`; the format line gains `"prompts"`: "(optional) the host data's pack (`open.json`'s host data with these `prompts`)".
- `CLAUDE.md`, Layout, src/turn: "hosted runs, a world the host owns and sends as data (`HostedWorld`, `HostedSession`)" becomes "... (`HostedWorld`, `HostedSession`, and `PromptPack`, the host's texts over the engine's defaults)".

ASCII only; check with `python -c "import sys;d=open(sys.argv[1],'rb').read();print(sum(c>127 for c in d))" <file>` on every changed file (expected 0).

- [ ] **Step 6: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp tests/hosted/HostedTestSupport.h tests/server/ServerTestSupport.h tests/hosted/fixtures/prompt_pack.json tests/server/ApiPromptTests.cpp tests/hosted/HostedFixtureTests.cpp tests/server/HttpMemoryTests.cpp tests/llama/LlamaHostedTests.cpp tests/server/ApiMemoryTests.cpp tests/server/ApiRewindTests.cpp tests/server/ApiTests.cpp tests/server/ApiTurnTests.cpp tests/server/HttpServerTests.cpp README.md CHANGELOG.md docs/superpowers/specs/2026-11-16-morpheus-server-design.md docs/superpowers/specs/2026-11-16-hosted-world-design.md tests/hosted/fixtures/README.md CLAUDE.md
git commit -m "Server: the pack through open, the defaults route, API 1.3.0; the prompt-pack fixture; docs"
```

---

## Finally (controller)

- `ctest --preset tests`; `cmake --build --preset asan` and `ctest --preset asan`.
- `"[turn]"`, `"[replay]"`, `"[context]"` pass (Morpheus's own game).
- `cmake --build --preset dev` builds clean; the consumer check (`cmake/consumer-check/`) is ok.
- Live, on the new defaults: `[llama][hosted]` in-process, and `[server]` over llama-server (README, "The [server] tests").
