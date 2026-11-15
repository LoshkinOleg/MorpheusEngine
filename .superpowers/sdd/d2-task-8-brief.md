### Task 8: `TurnStream` and the `Api`: every route but the turn

**Files:**
- Create: `src/server/TurnStream.h`, `src/server/TurnStream.cpp`, `src/server/Api.h`, `src/server/Api.cpp`
- Modify: `src/server/CMakeLists.txt` (add `Api.cpp`, `TurnStream.cpp`)
- Modify: `tests/server/ServerTestSupport.h` (add `ApiHarness`, `ReadAll`)
- Test: `tests/server/ApiTests.cpp`, `tests/server/TurnStreamTests.cpp`

**Interfaces:**
- Consumes: `server::RunRegistry`, `server::RunWorker` (Tasks 6, 7), `server::SlotPool`, `server::Log` (Task 4), `turn::HostDataFromJson`, `turn::HostedSession` (`StartConversation`, `RecordEvents`, `SetParticipants`, `EndConversation`, `Conversation`, `Turns`, `Events`), `turn::TurnError` (`src/turn/MemoryUpkeep.h`).
- Produces:
  - `struct server::StreamEvent { std::string name; nlohmann::json data; }` and `class server::TurnStream` with `void Push(StreamEvent event)`, `void End()`, `std::optional<StreamEvent> Next(std::chrono::milliseconds wait)`, `bool Ended() const`, `bool WaitStarted(std::chrono::milliseconds wait)`, `std::optional<StreamEvent> Front() const`, `void Cancel()`, `std::stop_token StopToken() const`.
  - `inline constexpr std::string_view server::API_VERSION = "1.0.0";`, `struct server::HealthInfo { std::string chatModel; std::string embeddingModel; int slots; }`, `struct server::ApiResponse { int status; nlohmann::json body; std::shared_ptr<TurnStream> stream; }`, `class server::Api` with `Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log)` and `ApiResponse Handle(std::string_view method, std::string_view path, std::string_view body)`.
  - In `Api.cpp`'s anonymous namespace, for Task 9: `class NotFound`, `class NotOpened`, `int StatusOf(const std::exception&)`, `void RequireConversation(turn::HostedSession&, std::string_view)`, `nlohmann::json ActionJson(const core::Adjudication&)`, `std::string SlotKey(std::string_view runId, std::string_view conversationId)`, `std::string RequiredString(const nlohmann::json&, std::string_view)`, `std::vector<std::string> StringList(const nlohmann::json&, std::string_view)`, `std::vector<nlohmann::json> ObjectList(const nlohmann::json&, std::string_view)`, `const nlohmann::json& RequiredObject(const nlohmann::json&, std::string_view)`.
  - Test helpers `test::ApiHarness` and `test::ReadAll`.

- [ ] **Step 1: Write the test helpers**

Add to `tests/server/ServerTestSupport.h` (new includes: `<atomic>`, `<mutex>`, `<optional>`, `<string_view>`, `<vector>`, `<nlohmann/json.hpp>`, `"TestPaths.h"`, `"inference/Llm.h"`, `"server/Api.h"`, `"server/RunRegistry.h"`, `"server/SlotPool.h"`, `"server/TurnStream.h"`):

```cpp
// Every event of `stream` until it ends, waiting at most 10 s in all.
inline std::vector<server::StreamEvent> ReadAll(server::TurnStream& stream)
{
    std::vector<server::StreamEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!stream.Ended() && std::chrono::steady_clock::now() < deadline) {
        if (std::optional<server::StreamEvent> event = stream.Next(std::chrono::milliseconds(50))) {
            events.push_back(std::move(*event));
        }
    }
    return events;
}

// An Api over runs in a fresh folder for test `name`: every run's backend is a scripted one answering with
// `responder` (pieceDelay before each streamed piece; backendDown makes new run backends fail to start), the utility
// backend counts and embeds as ScriptedBackend does, and the pool has 3 slots. Log lines are kept.
struct ApiHarness {
    std::filesystem::path runsDir = {};
    ScriptedBackend::Responder responder = {};
    std::chrono::milliseconds pieceDelay = std::chrono::milliseconds(0);
    std::atomic<bool> backendDown = false;
    std::mutex mutex = {};                     // Guards logLines and backends.
    std::vector<std::string> logLines = {};
    std::vector<ScriptedBackend*> backends = {}; // Each run's backend, in the order the runs opened.
    ScriptedBackend utility{16384};
    server::SlotPool slots{3};
    server::RunRegistry runs;
    server::Api api;

    ApiHarness(std::string_view name, ScriptedBackend::Responder responderIn)
        : runsDir(FreshTempDir(name) / "runs")
        , responder(std::move(responderIn))
        , runs(runsDir, HostedSettings(), [this] { return MakeBackend(); }, [this](const std::string& line) { Log(line); })
        , api(runs, slots, utility, {"chat-model", "embed-model", 3}, [this](const std::string& line) { Log(line); })
    {
    }

    std::unique_ptr<inference::LlmBackend> MakeBackend()
    {
        if (backendDown) {
            throw inference::InferenceError("cannot reach http://127.0.0.1:1/props");
        }
        auto pBackend = std::make_unique<ScriptedBackend>(16384, responder);
        pBackend->SetPieceDelay(pieceDelay);
        std::scoped_lock lock(mutex);
        backends.push_back(pBackend.get());
        return pBackend;
    }

    void Log(const std::string& line)
    {
        std::scoped_lock lock(mutex);
        logLines.push_back(line);
    }

    server::ApiResponse Post(const std::string& path, const nlohmann::json& body)
    {
        return api.Handle("POST", path, body.dump());
    }

    server::ApiResponse Get(const std::string& path)
    {
        return api.Handle("GET", path, "");
    }
};
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/TurnStreamTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <thread>

#include "core/Errors.h"
#include "server/TurnStream.h"

using namespace morpheus;
using namespace std::chrono_literals;

TEST_CASE("TurnStream hands events over in order, then says it ended", "[hosted-server][stream]")
{
    server::TurnStream stream;
    CHECK_FALSE(stream.WaitStarted(10ms));
    CHECK_FALSE(stream.Front().has_value());
    std::jthread worker([&stream] {
        stream.Push({"action", {{"speaker", "a"}}});
        stream.Push({"token", {{"text", "Hi"}}});
        stream.End();
    });
    CHECK(stream.WaitStarted(5000ms));
    REQUIRE(stream.Front().has_value());
    CHECK(stream.Front()->name == "action"); // Front does not take it.
    std::optional<server::StreamEvent> first = stream.Next(5000ms);
    REQUIRE(first);
    CHECK(first->name == "action");
    worker.join();
    std::optional<server::StreamEvent> second = stream.Next(5000ms);
    REQUIRE(second);
    CHECK(second->data.at("text") == "Hi");
    CHECK(stream.Ended());
    CHECK_FALSE(stream.Next(10ms).has_value());
    CHECK_THROWS_AS(stream.Push({"token", {}}), core::ContractError);
}

TEST_CASE("TurnStream::Cancel fires its stop token", "[hosted-server][stream]")
{
    server::TurnStream stream;
    const std::stop_token token = stream.StopToken();
    CHECK_FALSE(token.stop_requested());
    stream.Cancel();
    CHECK(token.stop_requested());
}
```

Create `tests/server/ApiTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// The body of a request starting a conversation of Hulda and Aela in observation.json.
nlohmann::json HuldaAndAela()
{
    return {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}};
}

// Checks that `response` is the error `status` with a message containing `part`, and the API version.
void CheckError(const server::ApiResponse& response, int status, const std::string& part)
{
    INFO(response.body.dump());
    CHECK(response.stream == nullptr);
    CHECK(response.status == status);
    CHECK(response.body.at("api_version") == "1.0.0");
    CHECK(response.body.at("error").at("status") == status);
    CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
}

} // namespace

TEST_CASE("/health reports the models, the slots and the API version", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_health", script.Responder());
    const server::ApiResponse health = harness.Get("/health");
    CHECK(health.status == 200);
    CHECK(health.body == nlohmann::json{{"ready", true},
                                        {"chat_model", "chat-model"},
                                        {"embedding_model", "embed-model"},
                                        {"slots", 3},
                                        {"api_version", "1.0.0"}});
}

TEST_CASE("Opening a run creates its file once; opening it again is idempotent; a seed goes to a new run",
          "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_open", script.Responder());
    const nlohmann::json open = test::ReadFixture("open.json");

    const server::ApiResponse first = harness.Post("/v1/runs/save_1/open", open);
    CHECK(first.status == 200);
    CHECK(first.body == nlohmann::json{{"run_id", "save_1"}, {"created", true}, {"api_version", "1.0.0"}});
    CHECK(std::filesystem::exists(harness.runsDir / "save_1.morpheus"));
    const server::ApiResponse second = harness.Post("/v1/runs/save_1/open", open);
    CHECK(second.status == 200);
    CHECK(second.body.at("created") == false);

    nlohmann::json seeded = open;
    seeded["seed"] = 7;
    REQUIRE(harness.Post("/v1/runs/save_2/open", seeded).status == 200);
    std::uint64_t seed = 0;
    harness.runs.Find("save_2")->Call([&seed](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
        seed = session.Meta().rngSeed;
    });
    CHECK(seed == 7);
    nlohmann::json negative = open;
    negative["seed"] = -1;
    CheckError(harness.Post("/v1/runs/save_3/open", negative), 400, "seed");
}

TEST_CASE("A conversation starts, records events, changes participants, ends, and reads back", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_conversation", script.Responder());
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);

    const server::ApiResponse started = harness.Post("/v1/runs/save_1/conversations", HuldaAndAela());
    REQUIRE(started.status == 200);
    CHECK(started.body == nlohmann::json{{"conversation_id", "c1"}, {"api_version", "1.0.0"}});
    const std::string base = "/v1/runs/save_1/conversations/c1";

    const server::ApiResponse recorded =
        harness.Post(base + "/events", {{"events", {{{"type", "ambient"}, {"text", "A door opens."}}}}});
    CHECK(recorded.status == 200);
    CHECK(recorded.body == nlohmann::json{{"api_version", "1.0.0"}});

    const server::ApiResponse joined =
        harness.Post(base + "/participants", {{"joined", {test::SAADIA}}, {"observation", test::FixtureObservation()}});
    CHECK(joined.status == 200);
    CHECK(joined.body.at("participants") == nlohmann::json{test::HULDA, test::AELA, test::SAADIA});
    const server::ApiResponse left =
        harness.Post(base + "/participants", {{"left", {test::AELA}}, {"observation", test::FixtureObservation()}});
    CHECK(left.body.at("participants") == nlohmann::json{test::HULDA, test::SAADIA});

    const server::ApiResponse read = harness.Get(base);
    REQUIRE(read.status == 200);
    const nlohmann::json& conversation = read.body.at("conversation");
    CHECK(conversation.at("id") == "c1");
    CHECK(conversation.at("ended") == false);
    CHECK(conversation.at("folded_through") == 0);
    REQUIRE(conversation.at("participants").size() == 2);
    CHECK(conversation.at("participants")[0].at("id") == test::HULDA);
    CHECK(conversation.at("participants")[0].at("name") == "Hulda");
    CHECK(read.body.at("turns") == nlohmann::json::array());
    REQUIRE(read.body.at("events").size() == 1);
    CHECK(read.body.at("events")[0].at("after_turn") == 0);
    CHECK(read.body.at("events")[0].at("event").at("text") == "A door opens.");

    CHECK(harness.Post(base + "/end", nlohmann::json::object()).status == 200);
    CHECK(harness.Get(base).body.at("conversation").at("ended") == true);
    CheckError(harness.Post(base + "/end", nlohmann::json::object()), 400, "has ended");
}

TEST_CASE("Errors carry their status, their message naming the fault, and the API version", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_errors", script.Responder());
    const nlohmann::json open = test::ReadFixture("open.json");
    REQUIRE(harness.Post("/v1/runs/save_1/open", open).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations", HuldaAndAela()).status == 200);
    const std::string c1 = "/v1/runs/save_1/conversations/c1";

    CheckError(harness.Get("/nope"), 404, "/nope");
    CheckError(harness.Post("/health", nlohmann::json::object()), 404, "/health");
    CheckError(harness.api.Handle("POST", "/v1/runs/save_1/open", "{not json"), 400, "body");
    CheckError(harness.api.Handle("POST", "/v1/runs/save_1/open", "[1, 2]"), 400, "body");
    CheckError(harness.Post("/v1/runs/a.b/open", open), 400, "run_id");
    CheckError(harness.Post("/v1/runs/save_9/conversations", HuldaAndAela()), 409, "save_9");
    CheckError(harness.Get("/v1/runs/save_9/conversations/c1"), 409, "save_9");
    CheckError(harness.Post("/v1/runs/save_2/open", {{"observation_schema", open.at("observation_schema")}}), 400,
               "host data at /catalog");
    CheckError(harness.Post("/v1/runs/save_1/conversations",
                            {{"participants", {"0000BEEF"}}, {"observation", test::FixtureObservation()}}),
               400, "participants/0");
    CheckError(harness.Post("/v1/runs/save_1/conversations", {{"participants", test::HULDA}}), 400, "participants");
    CheckError(harness.Post("/v1/runs/save_1/conversations", {{"participants", {test::HULDA}}}), 400, "observation");
    CheckError(harness.Get("/v1/runs/save_1/conversations/c9"), 404, "conversation_id");
    CheckError(harness.Post("/v1/runs/save_1/conversations/c9/events", {{"events", nlohmann::json::array()}}), 404,
               "c9");
    CheckError(harness.Post(c1 + "/events", {{"events", {{{"type", "ambient"}}}}}), 400, "events/0/text");
    CheckError(harness.Post(c1 + "/events", {{"events", {"not an object"}}}), 400, "events/0");
    CheckError(harness.Post(c1 + "/nope", nlohmann::json::object()), 404, "/nope");
    CheckError(harness.Post("/v1/tokenize", nlohmann::json::object()), 400, "content");
    CheckError(harness.Post("/v1/embeddings", {{"input", {"a"}}, {"purpose", "other"}}), 400, "purpose");

    // A backend that cannot start: 503.
    harness.backendDown = true;
    CheckError(harness.Post("/v1/runs/save_3/open", open), 503, "cannot reach");
}

TEST_CASE("/v1/tokenize and /v1/embeddings answer with the utility backend", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_utility", script.Responder());
    const server::ApiResponse tokens = harness.Post("/v1/tokenize", {{"content", "abcdefgh"}});
    CHECK(tokens.status == 200);
    CHECK(tokens.body.at("tokens") == harness.utility.CountTokens("abcdefgh"));

    const server::ApiResponse vectors =
        harness.Post("/v1/embeddings", {{"input", {"a lighthouse", "a keeper"}}, {"purpose", "query"}});
    CHECK(vectors.status == 200);
    CHECK(vectors.body.at("model") == harness.utility.EmbeddingModelId());
    REQUIRE(vectors.body.at("embeddings").size() == 2);
    CHECK(vectors.body.at("embeddings")[0].get<std::vector<float>>() ==
          harness.utility.Embed({"a lighthouse"}, inference::EmbedPurpose::Query)[0]);
}
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/Api.h'`.

- [ ] **Step 4: Write `TurnStream`**

Create `src/server/TurnStream.h`:

```cpp
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::server {

// One event of a turn's stream (morpheus_server spec, section 1, "The turn stream"): action, token, sentence, done or
// error, and its JSON.
struct StreamEvent {
    std::string name = {};
    nlohmann::json data = nlohmann::json::object();
};

// A turn's events on their way from the run's worker (Push, End) to the HTTP handler that writes them (Next, Cancel).
// The handler's side also tells the turn to stop: Cancel fires StopToken, which the turn runs under. Thread-safe.
class TurnStream {
private:
    mutable std::mutex _mutex = {};
    std::condition_variable _changed = {};
    std::deque<StreamEvent> _events = {};
    bool _ended = false;
    std::stop_source _stop = {};

public:
    // Queues an event. Throws ContractError after End.
    void Push(StreamEvent event);
    // No event follows.
    void End();
    // Takes the next event, waiting up to `wait` for one; nullopt when none came (Ended() tells whether one ever will).
    std::optional<StreamEvent> Next(std::chrono::milliseconds wait);
    // Whether the stream ended and every event was taken.
    bool Ended() const;
    // Waits up to `wait` for the stream to start: true once an event is queued or the stream ended.
    bool WaitStarted(std::chrono::milliseconds wait);
    // The next event without taking it; nullopt when none is queued.
    std::optional<StreamEvent> Front() const;
    // The client left, or the server stops: the turn's stop token fires.
    void Cancel();
    // What the turn runs under.
    std::stop_token StopToken() const;
};

} // namespace morpheus::server
```

Create `src/server/TurnStream.cpp`:

```cpp
#include "server/TurnStream.h"

#include <utility>

#include "core/Errors.h"

namespace morpheus::server {

void TurnStream::Push(StreamEvent event)
{
    {
        std::scoped_lock lock(_mutex);
        MORPHEUS_REQUIRE(!_ended, "TurnStream::Push: the stream has ended");
        _events.push_back(std::move(event));
    }
    _changed.notify_all();
}

void TurnStream::End()
{
    {
        std::scoped_lock lock(_mutex);
        _ended = true;
    }
    _changed.notify_all();
}

std::optional<StreamEvent> TurnStream::Next(std::chrono::milliseconds wait)
{
    std::unique_lock lock(_mutex);
    _changed.wait_for(lock, wait, [this] { return !_events.empty() || _ended; });
    if (_events.empty()) {
        return std::nullopt;
    }
    StreamEvent event = std::move(_events.front());
    _events.pop_front();
    return event;
}

bool TurnStream::Ended() const
{
    std::scoped_lock lock(_mutex);
    return _ended && _events.empty();
}

bool TurnStream::WaitStarted(std::chrono::milliseconds wait)
{
    std::unique_lock lock(_mutex);
    return _changed.wait_for(lock, wait, [this] { return !_events.empty() || _ended; });
}

std::optional<StreamEvent> TurnStream::Front() const
{
    std::scoped_lock lock(_mutex);
    if (_events.empty()) {
        return std::nullopt;
    }
    return _events.front();
}

void TurnStream::Cancel()
{
    _stop.request_stop();
}

std::stop_token TurnStream::StopToken() const
{
    return _stop.get_token();
}

} // namespace morpheus::server
```

- [ ] **Step 5: Write the `Api` without the turn route**

Create `src/server/Api.h`:

```cpp
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "inference/Llm.h"
#include "server/RunRegistry.h"
#include "server/ServerLog.h"
#include "server/SlotPool.h"
#include "server/TurnStream.h"

namespace morpheus::server {

// morpheus_server's API version, on every response (spec, section 1). A breaking change bumps the major version and
// gets a CHANGELOG.md entry.
inline constexpr std::string_view API_VERSION = "1.0.0";

// What /health reports about the models.
struct HealthInfo {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int slots = 0;
};

// One answer: a status and a JSON body, or, for a turn that started, the stream of its events.
struct ApiResponse {
    int status = 200;
    nlohmann::json body = nlohmann::json::object(); // Carries api_version. Unused when `stream` is set.
    std::shared_ptr<TurnStream> stream = nullptr;   // A started turn: its events, the first (action) already queued.
};

// morpheus_server's routes without sockets (spec, sections 1 and 4): a method, a path and a body in; a status and JSON,
// or a turn's stream, out. Every route, check and status lives here; HttpServer only carries bytes. A run's requests
// run on its worker (RunWorker), so requests of different runs proceed at once; /v1/tokenize and /v1/embeddings take
// turns on the utility backend. Thread-safe.
class Api {
private:
    RunRegistry* _pRuns = nullptr;              // Not owned; outlives this.
    SlotPool* _pSlots = nullptr;                // Not owned; outlives this.
    inference::LlmBackend* _pUtility = nullptr; // Not owned; outlives this. Used under _utilityMutex.
    std::mutex _utilityMutex = {};
    HealthInfo _health = {};
    Log _log = {};

public:
    // Throws ContractError for an empty log.
    Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log);

    // Handles one request: `method` ("GET", "POST", ...), `path` ("/v1/runs/save_1/open"), `body` (JSON text, or empty).
    // Never throws for the request's or the run's fault: every failure is its status and
    // {"api_version", "error": {"status", "message"}}; a 500 is logged too.
    ApiResponse Handle(std::string_view method, std::string_view path, std::string_view body);
};

} // namespace morpheus::server
```

Create `src/server/Api.cpp`:

```cpp
#include "server/Api.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/SlotBackend.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"
#include "turn/MemoryUpkeep.h"

namespace morpheus::server {

namespace {

// A request for something that does not exist: an unknown route, or an unknown conversation of an opened run (404).
class NotFound : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A run used before its open in this process: its host data comes with the open (409).
class NotOpened : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The status of a request that failed with `error` (spec, section 1, "Errors").
int StatusOf(const std::exception& error)
{
    if (dynamic_cast<const NotFound*>(&error) != nullptr) {
        return 404;
    }
    if (dynamic_cast<const NotOpened*>(&error) != nullptr) {
        return 409;
    }
    if (dynamic_cast<const core::DataError*>(&error) != nullptr) {
        return 400;
    }
    if (dynamic_cast<const turn::TurnError*>(&error) != nullptr) {
        return 422;
    }
    if (dynamic_cast<const inference::InferenceError*>(&error) != nullptr) {
        return 503;
    }
    return 500;
}

// Conversation `id`, which must exist in the run (NotFound otherwise); an ended one is HostedSession's DataError.
void RequireConversation(turn::HostedSession& session, std::string_view id)
{
    if (!session.Conversation(id)) {
        throw NotFound(std::format("conversation_id: there is no conversation '{}'", id));
    }
}

// An adjudication's action as the host reads it: {"id", "params"}, or null for a refused one.
nlohmann::json ActionJson(const core::Adjudication& adjudication)
{
    if (!adjudication.requested) {
        return nullptr;
    }
    return {{"id", adjudication.actionId}, {"params", adjudication.params}};
}

// A conversation's key in the slot pool: its run's id and its own.
std::string SlotKey(std::string_view runId, std::string_view conversationId)
{
    return std::format("{}/{}", runId, conversationId);
}

// The body's string `key`. DataError "<key>: ..." when it is missing or not a string.
std::string RequiredString(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) {
        throw core::DataError(std::format("{}: expected a string", key));
    }
    return it->get<std::string>();
}

// The body's array of strings `key`. DataError naming `key` or `key/<i>` when it is missing or malformed.
std::vector<std::string> StringList(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_array()) {
        throw core::DataError(std::format("{}: expected an array of strings", key));
    }
    std::vector<std::string> list;
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_string()) {
            throw core::DataError(std::format("{}/{}: expected a string", key, i));
        }
        list.push_back((*it)[i].get<std::string>());
    }
    return list;
}

// The body's array of objects `key` (events). DataError naming `key` or `key/<i>` when it is missing or malformed.
std::vector<nlohmann::json> ObjectList(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_array()) {
        throw core::DataError(std::format("{}: expected an array of objects", key));
    }
    std::vector<nlohmann::json> list;
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_object()) {
            throw core::DataError(std::format("{}/{}: expected an object", key, i));
        }
        list.push_back((*it)[i]);
    }
    return list;
}

// The body's object `key` (an observation). DataError "<key>: ..." when it is missing or not an object.
const nlohmann::json& RequiredObject(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_object()) {
        throw core::DataError(std::format("{}: expected an object", key));
    }
    return *it;
}

} // namespace

Api::Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log)
    : _pRuns(&runs)
    , _pSlots(&slots)
    , _pUtility(&utility)
    , _health(std::move(health))
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_log), "Api: a log is needed");
}

ApiResponse Api::Handle(std::string_view method, std::string_view path, std::string_view body)
{
    ApiResponse response;
    try {
        // The body: a JSON object, or nothing (an empty object).
        nlohmann::json json = nlohmann::json::object();
        if (!body.empty()) {
            json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded() || !json.is_object()) {
                throw core::DataError("body: expected a JSON object");
            }
        }

        // The path's segments: "/v1/runs/save_1/open" is {"v1", "runs", "save_1", "open"}.
        std::vector<std::string_view> parts;
        for (std::size_t start = 0; start < path.size();) {
            const std::size_t end = std::min(path.find('/', start), path.size());
            if (end > start) {
                parts.push_back(path.substr(start, end - start));
            }
            start = end + 1;
        }
        const bool get = method == "GET";
        const bool post = method == "POST";
        auto route = [&parts](std::initializer_list<std::string_view> shape) {
            return std::equal(parts.begin(), parts.end(), shape.begin(), shape.end());
        };
        auto unknown = [&] { return NotFound(std::format("there is no route {} {}", method, path)); };

        if (get && route({"health"})) {
            response.body = {{"ready", true},
                             {"chat_model", _health.chatModel},
                             {"embedding_model", _health.embeddingModel},
                             {"slots", _health.slots}};
        } else if (post && route({"v1", "tokenize"})) {
            // Exact counts from the chat model, for the host's own budgeting.
            const std::string content = RequiredString(json, "content");
            std::scoped_lock lock(_utilityMutex);
            response.body = {{"tokens", _pUtility->CountTokens(content)}};
        } else if (post && route({"v1", "embeddings"})) {
            const std::vector<std::string> input = StringList(json, "input");
            const std::string purpose = RequiredString(json, "purpose");
            if (purpose != "query" && purpose != "document") {
                throw core::DataError("purpose: expected \"query\" or \"document\"");
            }
            std::scoped_lock lock(_utilityMutex);
            response.body = {{"embeddings", _pUtility->Embed(input, purpose == "query" ? inference::EmbedPurpose::Query
                                                                                       : inference::EmbedPurpose::Document)},
                             {"model", _pUtility->EmbeddingModelId()}};
        } else if (parts.size() >= 4 && parts[0] == "v1" && parts[1] == "runs") {
            const std::string runId(parts[2]);
            RunRegistry::CheckRunId(runId);
            // The run's worker; a run not opened in this process has none (its host data comes with the open).
            auto worker = [this, &runId]() -> RunWorker& {
                RunWorker* pWorker = _pRuns->Find(runId);
                if (pWorker == nullptr) {
                    throw NotOpened(std::format("run '{}' is not open: open it first (POST /v1/runs/{}/open)", runId, runId));
                }
                return *pWorker;
            };

            if (post && parts.size() == 4 && parts[3] == "open") {
                // The host data, and an optional seed for a new run's sampling.
                std::uint64_t seed = 0;
                if (json.contains("seed")) {
                    if (!json.at("seed").is_number_unsigned()) {
                        throw core::DataError("seed: expected an integer of at least 0");
                    }
                    seed = json.at("seed").get<std::uint64_t>();
                    json.erase("seed");
                }
                const bool created = _pRuns->Open(runId, turn::HostDataFromJson(json), seed);
                response.body = {{"run_id", runId}, {"created", created}};
            } else if (post && parts.size() == 4 && parts[3] == "conversations") {
                const std::vector<std::string> participants = StringList(json, "participants");
                const nlohmann::json& observation = RequiredObject(json, "observation");
                std::string id;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    id = session.StartConversation(participants, observation);
                });
                response.body = {{"conversation_id", id}};
            } else if (parts.size() >= 5 && parts[3] == "conversations") {
                const std::string conversationId(parts[4]);
                if (get && parts.size() == 5) {
                    // The stored records, for debugging and the host's own tests.
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        const core::Conversation conversation = *session.Conversation(conversationId);
                        nlohmann::json participants = nlohmann::json::array();
                        for (const core::Participant& participant : conversation.participants) {
                            participants.push_back(
                                {{"id", participant.id}, {"name", participant.name}, {"bio", participant.bio}});
                        }
                        nlohmann::json turns = nlohmann::json::array();
                        for (const core::HostedTurn& turn : session.Turns(conversationId)) {
                            nlohmann::json entry = {
                                {"number", turn.number},
                                {"speaker", turn.speakerId},
                                {"listeners", turn.listenerIds},
                                {"player_text", turn.playerText ? nlohmann::json(*turn.playerText) : nlohmann::json(nullptr)},
                                {"events", turn.events},
                                {"observation", turn.observation},
                                {"resolve_output", turn.resolveOutput},
                                {"action", ActionJson(turn.adjudication)},
                                {"line", turn.line},
                                {"events_omitted", turn.eventsOmitted},
                                {"timings", turn.timings}};
                            if (!turn.adjudication.requested) {
                                entry["refused"] = turn.adjudication.reasons;
                            }
                            turns.push_back(std::move(entry));
                        }
                        nlohmann::json events = nlohmann::json::array();
                        for (const core::ConversationEvent& event : session.Events(conversationId)) {
                            events.push_back({{"after_turn", event.afterTurn}, {"event", event.event}});
                        }
                        response.body = {{"conversation",
                                          {{"id", conversation.id},
                                           {"participants", participants},
                                           {"ended", conversation.ended},
                                           {"folded_through", conversation.foldedThrough}}},
                                         {"turns", turns},
                                         {"events", events}};
                    });
                } else if (post && parts.size() == 6 && parts[5] == "events") {
                    const std::vector<nlohmann::json> events = ObjectList(json, "events");
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.RecordEvents(conversationId, events);
                    });
                } else if (post && parts.size() == 6 && parts[5] == "participants") {
                    const std::vector<std::string> joined =
                        json.contains("joined") ? StringList(json, "joined") : std::vector<std::string>();
                    const std::vector<std::string> left =
                        json.contains("left") ? StringList(json, "left") : std::vector<std::string>();
                    const nlohmann::json& observation = RequiredObject(json, "observation");
                    nlohmann::json ids = nlohmann::json::array();
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.SetParticipants(conversationId, joined, left, observation);
                        for (const core::Participant& participant : session.Conversation(conversationId)->participants) {
                            ids.push_back(participant.id);
                        }
                    });
                    response.body = {{"participants", ids}};
                } else if (post && parts.size() == 6 && parts[5] == "end") {
                    // The conversation ends; its slot is free for the next one, and folding follows in the background.
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.EndConversation(conversationId);
                    });
                    _pSlots->Release(SlotKey(runId, conversationId));
                } else {
                    throw unknown();
                }
            } else {
                throw unknown();
            }
        } else {
            throw unknown();
        }
    } catch (const std::exception& error) {
        // Every failure is its status and message; a 500 is a bug or damaged data, so the operator sees it too.
        const int status = StatusOf(error);
        if (status == 500) {
            _log(std::format("{} {} failed: {}", method, path, error.what()));
        }
        response = ApiResponse{status, {{"error", {{"status", status}, {"message", error.what()}}}}, nullptr};
    }
    if (!response.stream) {
        response.body["api_version"] = std::string(API_VERSION);
    }
    return response;
}

} // namespace morpheus::server
```

Notes for the implementer:
- `ActionJson` and `SlotKey` each have a second caller in Task 9; until then MSVC may warn that `ActionJson` is used once (it is not: the GET route uses it), and `SlotKey` is used by the end route. `StatusOf` and `RequireConversation` are used here and in Task 9.
- `std::equal` and `std::min` need `<algorithm>`; `std::initializer_list` needs `<initializer_list>`.
- `nlohmann::json::find(std::string_view)` works with nlohmann 3.12; if it does not compile, use `body.find(std::string(key))`.

Add `Api.cpp` and `TurnStream.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[api],[stream]"`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/server/ tests/server/
git commit -m "Api and TurnStream: health, open, conversations, events, participants, end, tokenize and embeddings, with every error's status"
```

---

