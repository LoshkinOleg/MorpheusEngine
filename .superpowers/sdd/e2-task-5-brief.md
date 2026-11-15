### Task 5: The server's rewind route, API 1.2.0, the reload fixture over HTTP

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp`
- Modify: `CHANGELOG.md`, `tests/server/ServerTestSupport.h` (the HTTP fixture runner's new steps)
- Test: `tests/server/ApiRewindTests.cpp` (new), `tests/server/HttpMemoryTests.cpp`, every server test naming `"1.1.0"`

**Interfaces:**
- Consumes: `HostedSession::Rewind`, `RewindResult` (Task 4).
- Produces: `POST /v1/runs/{run_id}/rewind` with `{game_time}` -> `{rewound, hidden_turns, restored_turns, rollback}`; `server::API_VERSION == "1.2.0"`; `PlayMemoryFixtureOverHttp` supports the `rewind`, `turns` and `conversation` steps and step clocks.

- [ ] **Step 1: Write the failing tests**

Change every `"1.1.0"` in `tests/server/` to `"1.2.0"`.

Create `tests/server/ApiRewindTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The rewind route answers what it hid and the open rollback", "[hosted-server][api][rewind]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})", R"({"action":"none","params":{}})"};
    script.lines = {"One.", "Two."};
    test::ApiHarness harness("api_rewind", test::MemoryEchoResponder(script));
    nlohmann::json open = test::ReadFixture("open.json");
    open["clock"] = "/game_time/days_passed";
    REQUIRE(harness.Post("/v1/runs/save_1/open", open).status == 200);
    nlohmann::json at1 = test::FixtureObservation();
    at1["game_time"]["days_passed"] = 1.0;
    nlohmann::json at2 = at1;
    at2["game_time"]["days_passed"] = 2.0;
    REQUIRE(harness.Post("/v1/runs/save_1/conversations", {{"participants", {test::HULDA, test::AELA}}, {"observation", at1}})
                .status == 200);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                {{"speaker", test::HULDA}, {"player_text", "Hi."}, {"observation", at1}})
                       .stream);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                {{"speaker", test::HULDA}, {"player_text", "Again."}, {"observation", at2}})
                       .stream);

    const server::ApiResponse back = harness.Post("/v1/runs/save_1/rewind", {{"game_time", 1.5}});
    CHECK(back.status == 200);
    CHECK(back.body == nlohmann::json{{"rewound", true},
                                      {"hidden_turns", 1},
                                      {"restored_turns", 0},
                                      {"rollback", {{"game_time", 1.5}}},
                                      {"api_version", "1.2.0"}});
    const server::ApiResponse forward = harness.Post("/v1/runs/save_1/rewind", {{"game_time", 5.0}});
    CHECK(forward.body.at("restored_turns") == 1);
    CHECK(forward.body.at("rollback").is_null());
}

TEST_CASE("The rewind route's errors", "[hosted-server][api][rewind]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_rewind_errors", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    auto check = [](const server::ApiResponse& response, int status, const std::string& part) {
        INFO(response.body.dump());
        CHECK(response.status == status);
        CHECK(response.body.at("api_version") == "1.2.0");
        CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
    };
    check(harness.Post("/v1/runs/save_1/rewind", {{"game_time", 1.0}}), 400, "no clock");
    check(harness.Post("/v1/runs/save_1/rewind", {{"game_time", "noon"}}), 400, "game_time");
    check(harness.Post("/v1/runs/save_1/rewind", nlohmann::json::object()), 400, "game_time");
    check(harness.Post("/v1/runs/save_9/rewind", {{"game_time", 1.0}}), 409, "save_9");
}
```

In `tests/server/HttpMemoryTests.cpp`, add:

```cpp
TEST_CASE("The reload fixture plays over HTTP", "[hosted-server][http][rewind][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("reload.json");
    test::HostedScript script = test::ScriptOf(fixture);
    test::ApiHarness harness("http_reload_fixture", test::MemoryEchoResponder(script));
    server::HttpServer http(harness.api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    test::PlayMemoryFixtureOverHttp(client, "r", fixture, true);
    harness.runs.StopAll();
    http.Stop();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: the new tests FAIL (404 for the route; "1.1.0" answered).

- [ ] **Step 3: The route and the runner**

In `src/server/Api.h`: `API_VERSION = "1.2.0"`.

In `src/server/Api.cpp`, inside the `/v1/runs/{run_id}` chain:

```cpp
            } else if (post && parts.size() == 4 && parts[3] == "rewind") {
                // The run as it was at a save's game time (rollback spec, section 3).
                const auto field = json.find("game_time");
                if (field == json.end() || !field->is_number()) {
                    throw core::DataError("game_time: expected a number");
                }
                const double gameTime = field->get<double>();
                turn::RewindResult result;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    result = session.Rewind(gameTime);
                });
                response.body = {{"rewound", result.rewound},
                                 {"hidden_turns", result.hiddenTurns},
                                 {"restored_turns", result.restoredTurns},
                                 {"rollback", result.rollbackTime ? nlohmann::json{{"game_time", *result.rollbackTime}}
                                                                  : nlohmann::json(nullptr)}};
```

In `tests/server/ServerTestSupport.h`, `PlayMemoryFixtureOverHttp`: the fixture's `clock` goes into the open body; step clocks go into each request's observation; `rewind` posts the route and checks the given fields; `turns` reads `GET .../conversations/{cid}` and checks `turns.size()`; `conversation` checks `conversation.ended`, the participants' ids and `events.size()` from the same GET; `events` posts `{"events": [...]}` to `.../conversations/{cid}/events`. (The memories checks keep waiting for the background fold, as they do.)

`CHANGELOG.md`, at the top:

```markdown
## API 1.2.0

Rolling a run back (design: `docs/superpowers/specs/2026-11-16-rollback-design.md`). Only additions.

- The host data may name a `clock`: a JSON pointer to a number in every observation (the game time).
- `POST /v1/runs/{run_id}/rewind` with `{game_time}` answers `{rewound, hidden_turns, restored_turns, rollback}`: the
  run as it was at that game time. Call it on every save load. What came later is hidden, not deleted; a later rewind
  restores it until a new record (a turn, events, a conversation, participants, an end, a share) abandons it.
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"` three times, then `cmake --build --preset asan` and `./build/asan/bin/Debug/morpheus_tests.exe "[hosted-server]"`, then `ctest --preset tests`.
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp CHANGELOG.md tests/server/
git commit -m "morpheus_server API 1.2.0: rewind a run to a save's game time; the reload fixture over HTTP"
```

---

