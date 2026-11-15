### Task 7: The server: share and memories routes, API 1.1.0, the three-actor fixture over HTTP

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp` (two routes)
- Modify: `CHANGELOG.md`
- Test: `tests/server/ApiMemoryTests.cpp` (new), `tests/server/HttpMemoryTests.cpp` (new), and every server test that names `"1.0.0"`

**Interfaces:**
- Consumes: `HostedSession::ShareMemories`, `ShareResult`, `HostedSession::Memories` (Tasks 4-6); the server's `Api` helpers (`RequireConversation`, `RequiredString`, `NotFound`, `StatusOf`) and test helpers (`test::ApiHarness`, `test::HttpServer` use, `test::PostTurn`, `test::JsonPost`, `test::JsonGet`, `test::WaitUntil`) from D2.
- Produces: `POST /v1/runs/{run_id}/memories/share` with `{from, to, conversation_id}` -> `{shared, pending}`; `GET /v1/runs/{run_id}/actors/{actor_id}/memories` -> `{memories: [{conversation_id, from_turn, to_turn, text, told_by, superseded}]}` oldest first; `server::API_VERSION == "1.1.0"`.

- [ ] **Step 1: Write the failing tests**

Change every `"1.0.0"` in `tests/server/` to `"1.1.0"` (`grep -rln '"1.0.0"' tests/server`).

Create `tests/server/ApiMemoryTests.cpp`:

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

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// Hulda's memories as GET answers them.
nlohmann::json HuldaMemories(test::ApiHarness& harness)
{
    return harness.Get("/v1/runs/save_1/actors/" + test::HULDA + "/memories").body.at("memories");
}

} // namespace

TEST_CASE("The memories of an actor read back, and a share copies them, told by the teller", "[hosted-server][api][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"The well is poisoned."};
    test::ApiHarness harness("api_memory_share", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    test::ReadAll(*harness
                       .Post("/v1/runs/save_1/conversations/c1/turn",
                             {{"speaker", test::HULDA}, {"player_text", "News?"}, {"observation", test::FixtureObservation()}})
                       .stream);

    // A share while the conversation is open waits.
    const server::ApiResponse early = harness.Post("/v1/runs/save_1/memories/share",
                                                   {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}});
    CHECK(early.status == 200);
    CHECK(early.body == nlohmann::json{{"shared", 0}, {"pending", true}, {"api_version", "1.1.0"}});

    // Ended, the conversation is folded in the background; the pending share then applies.
    REQUIRE(harness.Post("/v1/runs/save_1/conversations/c1/end", nlohmann::json::object()).status == 200);
    REQUIRE(test::WaitUntil([&] {
        return harness.Get("/v1/runs/save_1/actors/" + test::SAADIA + "/memories").body.at("memories").size() == 1;
    }));
    const nlohmann::json hulda = HuldaMemories(harness);
    REQUIRE(hulda.size() == 1);
    CHECK(hulda[0].at("conversation_id") == "c1");
    CHECK(hulda[0].at("told_by") == "");
    CHECK(hulda[0].at("superseded") == false);
    CHECK_THAT(hulda[0].at("text").get<std::string>(), ContainsSubstring("poisoned"));
    const nlohmann::json saadia = harness.Get("/v1/runs/save_1/actors/" + test::SAADIA + "/memories").body.at("memories");
    CHECK(saadia[0].at("told_by") == test::HULDA);
    CHECK(saadia[0].at("from_turn") == 1);
    CHECK(saadia[0].at("to_turn") == 1);

    // Sharing again copies nothing new.
    const server::ApiResponse again = harness.Post("/v1/runs/save_1/memories/share",
                                                   {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}});
    CHECK(again.body.at("shared") == 0);
    CHECK(again.body.at("pending") == false);
}

TEST_CASE("Memory routes answer their errors with the API version", "[hosted-server][api][memory]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_memory_errors", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    auto check = [](const server::ApiResponse& response, int status, const std::string& part) {
        INFO(response.body.dump());
        CHECK(response.status == status);
        CHECK(response.body.at("api_version") == "1.1.0");
        CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
    };
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c9"}}),
          404, "c9");
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::SAADIA}, {"to", test::HULDA}, {"conversation_id", "c1"}}),
          400, "from");
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::HULDA}, {"conversation_id", "c1"}}), 400, "to");
    check(harness.Post("/v1/runs/save_9/memories/share", {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}}),
          409, "save_9");
    check(harness.Get("/v1/runs/save_9/actors/" + test::HULDA + "/memories"), 409, "save_9");
    // An actor with no memories: an empty list.
    const server::ApiResponse none = harness.Get("/v1/runs/save_1/actors/0000BEEF/memories");
    CHECK(none.status == 200);
    CHECK(none.body.at("memories") == nlohmann::json::array());
}
```

(Saadia never witnessed `c1`'s turn, so she is refused as a teller with "from". The first share's 400 for `from` comes from `ShareMemories` before the turn exists: if `c1` has no turn yet, `NpcWitnesses` is empty and every teller is refused, which this test relies on; it runs no turn.)

Create `tests/server/HttpMemoryTests.cpp`: the three-actor fixture over real sockets, on scripted backends (the fixture's memory checks need the echo responder):

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/HttpServer.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The three-actor fixture plays over HTTP", "[hosted-server][http][memory][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("three_actors.json");
    const nlohmann::json& observation = fixture.at("observation");
    test::HostedScript script;
    for (const nlohmann::json& step : fixture.at("steps")) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    test::ApiHarness harness("http_memory_fixture", test::MemoryEchoResponder(script));
    server::HttpServer http(harness.api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    REQUIRE(test::JsonPost(client, "/v1/runs/r/open", test::ReadFixture("open.json")).status == 200);
    auto start = [&](const nlohmann::json& participants) {
        const test::HttpJson started =
            test::JsonPost(client, "/v1/runs/r/conversations", {{"participants", participants}, {"observation", observation}});
        REQUIRE(started.status == 200);
        return started.body.at("conversation_id").get<std::string>();
    };
    std::string id = start(fixture.at("participants"));
    const std::string firstId = id;
    // An actor's memories as the server answers them.
    auto memoriesOf = [&](const std::string& actor) {
        return test::JsonGet(client, "/v1/runs/r/actors/" + actor + "/memories").body.at("memories");
    };

    for (std::size_t i = 0; i < fixture.at("steps").size(); ++i) {
        const nlohmann::json& step = fixture.at("steps")[i];
        INFO("step " << i);
        const std::string base = "/v1/runs/r/conversations/" + id;
        if (step.contains("turn")) {
            nlohmann::json body = step.at("turn");
            body["observation"] = observation;
            const test::HttpTurn turn = test::PostTurn(client, base + "/turn", body);
            REQUIRE(turn.status == 200);
            REQUIRE_FALSE(turn.events.empty());
            CHECK(turn.events.back().name == "done"); // The prompt checks need the session: HostedFixtureTests.
        } else if (step.contains("leave")) {
            CHECK(test::JsonPost(client, base + "/participants", {{"left", step.at("leave")}, {"observation", observation}})
                      .status == 200);
        } else if (step.contains("join")) {
            CHECK(test::JsonPost(client, base + "/participants", {{"joined", step.at("join")}, {"observation", observation}})
                      .status == 200);
        } else if (step.contains("end")) {
            CHECK(test::JsonPost(client, base + "/end", nlohmann::json::object()).status == 200);
        } else if (step.contains("maintain")) {
            // The server folds in the background: the memories checks below wait for what they expect.
        } else if (step.contains("memories")) {
            const nlohmann::json& check = step.at("memories");
            const std::string actor = check.at("actor").get<std::string>();
            // Every memory's text, once the expected ones have arrived.
            auto texts = [&] {
                std::string all;
                for (const nlohmann::json& memory : memoriesOf(actor)) {
                    all += memory.at("text").get<std::string>() + "\n";
                }
                return all;
            };
            const nlohmann::json contains = check.value("contains", nlohmann::json::array());
            CHECK(test::WaitUntil([&] {
                const std::string all = texts();
                for (const nlohmann::json& present : contains) {
                    if (all.find(present.get<std::string>()) == std::string::npos) {
                        return false;
                    }
                }
                return true;
            }));
            const std::string all = texts();
            for (const nlohmann::json& absent : check.value("absent", nlohmann::json::array())) {
                CHECK_THAT(all, !ContainsSubstring(absent.get<std::string>()));
            }
            if (check.contains("count")) {
                CHECK(memoriesOf(actor).size() == check.at("count").get<std::size_t>());
            }
            for (const auto& item : check.value("told_by", nlohmann::json::object()).items()) {
                bool found = false;
                for (const nlohmann::json& memory : memoriesOf(actor)) {
                    if (memory.at("text").get<std::string>().find(item.key()) != std::string::npos) {
                        found = true;
                        CHECK(memory.at("told_by") == item.value());
                    }
                }
                CHECK(found);
            }
        } else if (step.contains("start")) {
            id = start(step.at("start"));
        } else if (step.contains("share")) {
            const nlohmann::json& share = step.at("share");
            const test::HttpJson shared = test::JsonPost(client, "/v1/runs/r/memories/share",
                                                         {{"from", share.at("from")},
                                                          {"to", share.at("to")},
                                                          {"conversation_id", share.value("conversation", firstId)}});
            REQUIRE(shared.status == 200);
            if (share.contains("shared")) {
                CHECK(shared.body.at("shared") == share.at("shared"));
            }
            if (share.contains("pending")) {
                CHECK(shared.body.at("pending") == share.at("pending"));
            }
        } else {
            FAIL("unknown step kind");
        }
    }
    harness.runs.StopAll();
    http.Stop();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: the new tests FAIL (404 "there is no route" for the two memory routes) and the renamed version checks FAIL ("1.0.0" answered).

- [ ] **Step 3: The routes**

In `src/server/Api.h`: `inline constexpr std::string_view API_VERSION = "1.1.0";`. Update the class comment's route list if it has one.

In `src/server/Api.cpp`, `Handle`, inside the `/v1/runs/{run_id}` branch, before the `conversations` branches:

```cpp
            } else if (post && parts.size() == 5 && parts[3] == "memories" && parts[4] == "share") {
                // One NPC tells another what it remembers of a conversation (per-speaker memory spec, section 2).
                const std::string from = RequiredString(json, "from");
                const std::string to = RequiredString(json, "to");
                const std::string conversationId = RequiredString(json, "conversation_id");
                turn::ShareResult result;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    RequireConversation(session, conversationId);
                    result = session.ShareMemories(from, to, conversationId);
                });
                response.body = {{"shared", result.shared}, {"pending", result.pending}};
            } else if (get && parts.size() == 6 && parts[3] == "actors" && parts[5] == "memories") {
                // Every memory of an actor, oldest first, for debugging and the host's own tests.
                const std::string actorId(parts[4]);
                nlohmann::json memories = nlohmann::json::array();
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    for (const core::Summary& memory : session.Memories(actorId)) {
                        memories.push_back({{"conversation_id", memory.conversationId},
                                            {"from_turn", memory.fromTurn},
                                            {"to_turn", memory.toTurn},
                                            {"text", memory.text},
                                            {"told_by", memory.toldBy},
                                            {"superseded", memory.superseded}});
                    }
                });
                response.body = {{"memories", memories}};
```

(Insert them so the existing `if / else if` chain stays one chain; check the branch order against the current code, which also routes `open` and `conversations` under `/v1/runs/{run_id}`. The `to` check of a missing field is `RequiredString`'s 400 "to: expected a string". `RunRegistry`'s 409 for an unopened run applies through `worker()`.)

Add the API 1.1.0 entry at the top of `CHANGELOG.md`:

```markdown
## API 1.1.0

Per-speaker memory (design: `docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md`). Only additions.

- `POST /v1/runs/{run_id}/memories/share` with `{from, to, conversation_id}` answers `{shared, pending}`: `to` learns
  what `from` remembers of the conversation, as copies marked told by `from`; a share waits (`pending`) until the
  conversation ended and `from`'s memory of it is complete.
- `GET /v1/runs/{run_id}/actors/{actor_id}/memories` answers `{memories: [{conversation_id, from_turn, to_turn, text,
  told_by, superseded}]}`, oldest first.
- Every NPC now remembers only what it witnessed; a turn's prompt shows the speaker's own memories.
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"` three times in a row, then `cmake --build --preset asan` and `./build/asan/bin/Debug/morpheus_tests.exe "[hosted-server]"`.
Expected: `All tests passed` each time.

- [ ] **Step 5: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp CHANGELOG.md tests/server/
git commit -m "morpheus_server API 1.1.0: share an NPC's memories of a conversation, read an actor's memories; the three-actor fixture over HTTP"
```

---

