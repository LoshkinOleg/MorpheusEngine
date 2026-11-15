### Task 10: `HttpServer`, and the fixtures over real HTTP

**Files:**
- Create: `src/server/HttpServer.h`, `src/server/HttpServer.cpp`
- Modify: `src/server/CMakeLists.txt` (add `HttpServer.cpp`)
- Modify: `tests/server/ServerTestSupport.h` (add `ServerConfigFor`, `HttpHarness`, `HttpTurn`, `PostTurn`, `JsonPost`, `JsonGet`, `Pieces`)
- Test: `tests/server/HttpServerTests.cpp`

**Interfaces:**
- Consumes: `server::Api`, `server::ApiResponse`, `server::TurnStream`, `server::StreamEvent` (Task 8), `llamaserver::LlamaServerBackend` with `SlotCount()` and `ChatModelId()` (Task 3), `test::FakeLlamaServer` (`tests/fakes/FakeLlamaServer.h`), `test::ConfigFor` (`tests/llamaserver/ServerTestSupport.h`), the hosted fixtures (`tests/hosted/fixtures/`).
- Produces: `class server::HttpServer` with `explicit HttpServer(Api& api)`, `~HttpServer()`, `void Start(int port)` (0 picks a free port; `core::DataError` naming the port when it cannot be bound), `int Port() const`, `void Stop()`. Test helpers `test::HttpHarness`, `test::HttpTurn`, `test::PostTurn`, `test::JsonPost`, `test::JsonGet`, `test::Pieces`, `test::ServerConfigFor`.

- [ ] **Step 1: Write the test helpers**

Add to `tests/server/ServerTestSupport.h` (new includes: `<sstream>`, `"fakes/FakeLlamaServer.h"`, `"llamaserver/LlamaServerBackend.h"`, `"llamaserver/ServerTestSupport.h"`, `"server/HttpServer.h"`; `<httplib.h>` comes through `fakes/FakeLlamaServer.h`):

```cpp
// A backend config on fake servers `chat` and `embed` for a whole server: calls left at -1 go to the upkeep slot, and
// the context is HostedSettings' 16384 (the fakes hold 16384 per slot).
inline llamaserver::LlamaServerConfig ServerConfigFor(const FakeLlamaServer& chat, const FakeLlamaServer& embed)
{
    llamaserver::LlamaServerConfig config = ConfigFor(chat, embed);
    config.slot = server::SlotPool::UPKEEP_SLOT;
    config.contextSize = 16384;
    return config;
}

// `text` in pieces of 4 bytes, as a fake server streams a completion. The fixtures' texts are ASCII.
inline std::vector<std::string> Pieces(const std::string& text)
{
    std::vector<std::string> pieces;
    for (std::size_t at = 0; at < text.size(); at += 4) {
        pieces.push_back(text.substr(at, 4));
    }
    return pieces;
}

// The whole server over real sockets, with fake llama-servers behind it, for test `name`: each run's backend and the
// utility backend on the fakes, HostedSettings, and the server on a free port of 127.0.0.1. Log lines are dropped.
struct HttpHarness {
    FakeLlamaServer chat;
    FakeLlamaServer embed;
    std::filesystem::path runsDir = {};
    llamaserver::LlamaServerBackend utility;
    server::SlotPool slots;
    server::RunRegistry runs;
    server::Api api;
    server::HttpServer http;

    explicit HttpHarness(std::string_view name)
        : runsDir(FreshTempDir(name) / "runs")
        , utility(ServerConfigFor(chat, embed))
        , slots(utility.SlotCount())
        , runs(runsDir, HostedSettings(),
               [this] { return std::make_unique<llamaserver::LlamaServerBackend>(ServerConfigFor(chat, embed)); },
               [](const std::string&) {})
        , api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
              [](const std::string&) {})
        , http(api)
    {
        http.Start(0);
    }

    // Running turns stop first, so the handlers waiting on them return; then the server stops.
    ~HttpHarness()
    {
        runs.StopAll();
        http.Stop();
    }

    httplib::Client Client() const
    {
        return httplib::Client("127.0.0.1", http.Port());
    }
};

// A JSON answer over HTTP: its status, its Content-Type and its body.
struct HttpJson {
    int status = 0;
    std::string contentType = {};
    nlohmann::json body = nullptr;
};

inline HttpJson JsonPost(httplib::Client& client, const std::string& path, const nlohmann::json& body)
{
    const httplib::Result result = client.Post(path, body.dump(), "application/json");
    if (!result) {
        return {};
    }
    return {result->status, result->get_header_value("Content-Type"), nlohmann::json::parse(result->body, nullptr, false)};
}

inline HttpJson JsonGet(httplib::Client& client, const std::string& path)
{
    const httplib::Result result = client.Get(path);
    if (!result) {
        return {};
    }
    return {result->status, result->get_header_value("Content-Type"), nlohmann::json::parse(result->body, nullptr, false)};
}

// A turn over HTTP: the status and Content-Type, the stream's events parsed back ("event:" and "data:" lines), or
// the JSON body of a refused turn.
struct HttpTurn {
    int status = 0;
    std::string contentType = {};
    std::vector<server::StreamEvent> events = {};
    nlohmann::json body = nullptr;
};

// Posts a turn and reads its answer. `keepReading`, when given, sees each event as it arrives; returning false
// closes the connection there (a client that leaves mid-stream; the status is then 0).
inline HttpTurn PostTurn(httplib::Client& client, const std::string& path, const nlohmann::json& body,
                         const std::function<bool(const server::StreamEvent&)>& keepReading = {})
{
    HttpTurn turn;
    std::string raw;     // Every byte received.
    std::string pending; // Bytes not yet parsed into events.
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.body = body.dump();
    request.set_header("Content-Type", "application/json");
    request.content_receiver = [&](const char* pData, std::size_t length, std::size_t, std::size_t) {
        raw.append(pData, length);
        pending.append(pData, length);
        // Each event ends with a blank line.
        for (std::size_t end = pending.find("\n\n"); end != std::string::npos; end = pending.find("\n\n")) {
            std::istringstream lines(pending.substr(0, end));
            pending.erase(0, end + 2);
            server::StreamEvent event;
            for (std::string line; std::getline(lines, line);) {
                if (line.starts_with("event: ")) {
                    event.name = line.substr(7);
                } else if (line.starts_with("data: ")) {
                    event.data = nlohmann::json::parse(line.substr(6));
                }
            }
            turn.events.push_back(event);
            if (keepReading && !keepReading(event)) {
                return false;
            }
        }
        return true;
    };
    const httplib::Result result = client.send(request);
    if (result) {
        turn.status = result->status;
        turn.contentType = result->get_header_value("Content-Type");
        // cpp-httplib may hand an error answer's body to the receiver or keep it in the response.
        const std::string& text = raw.empty() ? result->body : raw;
        if (turn.events.empty() && !text.empty()) {
            turn.body = nlohmann::json::parse(text, nullptr, false);
        }
    }
    return turn;
}
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/HttpServerTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "fakes/FakeLlamaServer.h"
#include "hosted/HostedTestSupport.h"
#include "server/HttpServer.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// The fixture's turn object as a request body: with observation.json when it has none.
nlohmann::json TurnBody(const nlohmann::json& turnJson)
{
    nlohmann::json body = turnJson;
    if (!body.contains("observation")) {
        body["observation"] = test::FixtureObservation();
    }
    return body;
}

// Replays conversation fixture `name` over HTTP: the fake chat server answers each turn's resolve and speak calls as
// the fixture's model says, each step goes as its request, and each turn's stream and the stored turns are checked
// against its "expect".
void ReplayOverHttp(const std::string& name)
{
    test::HttpHarness harness("http_fixture_" + name);
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/fixture/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture(name);
    const test::HttpJson started = test::JsonPost(
        client, "/v1/runs/fixture/conversations",
        {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}});
    REQUIRE(started.status == 200);
    const std::string base = "/v1/runs/fixture/conversations/" + started.body.at("conversation_id").get<std::string>();

    std::vector<nlohmann::json> expected; // Each turn step's "expect", in order.
    for (std::size_t i = 0; i < fixture.at("steps").size(); ++i) {
        const nlohmann::json& step = fixture.at("steps")[i];
        INFO(name << " step " << i);
        if (step.contains("events")) {
            CHECK(test::JsonPost(client, base + "/events", {{"events", step.at("events")}}).status == 200);
            continue;
        }
        harness.chat.QueueCompletion({test::Pieces(step.at("model").at("resolve").get<std::string>())});
        harness.chat.QueueCompletion({test::Pieces(step.at("model").at("line").get<std::string>())});
        const test::HttpTurn turn = test::PostTurn(client, base + "/turn", TurnBody(step.at("turn")));
        REQUIRE(turn.status == 200);
        CHECK_THAT(turn.contentType, StartsWith("text/event-stream"));
        REQUIRE(turn.events.size() >= 2);
        expected.push_back(step.at("expect"));

        // The stream: the action first with the API version, then tokens and sentences, done last.
        const nlohmann::json& expect = step.at("expect");
        const nlohmann::json& action = turn.events.front().data;
        CHECK(turn.events.front().name == "action");
        CHECK(action.at("api_version") == "1.0.0");
        CHECK(action.at("action").is_null() == !expect.at("requested").get<bool>());
        if (expect.contains("action")) {
            CHECK(action.at("action") == expect.at("action"));
        }
        if (expect.contains("reasons")) {
            CHECK(action.at("refused") == expect.at("reasons"));
        }
        std::string tokens;
        for (const server::StreamEvent& event : turn.events) {
            if (event.name == "token") {
                tokens += event.data.at("text").get<std::string>();
            }
        }
        CHECK(tokens == step.at("model").at("line").get<std::string>());
        CHECK(turn.events.back().name == "done");
        CHECK(turn.events.back().data.at("line") == expect.at("line"));
        CHECK(turn.events.back().data.at("turn") == static_cast<int>(expected.size()));
    }

    // The stored turns, through GET.
    const test::HttpJson stored = test::JsonGet(client, base);
    REQUIRE(stored.status == 200);
    const nlohmann::json& turns = stored.body.at("turns");
    REQUIRE(turns.size() == expected.size());
    for (std::size_t i = 0; i < turns.size(); ++i) {
        CHECK(turns[i].at("line") == expected[i].at("line"));
        CHECK(turns[i].at("listeners") == expected[i].at("listeners"));
    }
}

} // namespace

TEST_CASE("Over HTTP every answer is JSON with the API version, and an unknown route is a 404", "[hosted-server][http]")
{
    test::HttpHarness harness("http_health");
    httplib::Client client = harness.Client();
    const test::HttpJson health = test::JsonGet(client, "/health");
    CHECK(health.status == 200);
    CHECK_THAT(health.contentType, StartsWith("application/json"));
    CHECK(health.body.at("api_version") == "1.0.0");
    CHECK(health.body.at("chat_model") == "nomic-embed-text-v1.5.Q8_0"); // The fake names one model file for both.
    CHECK(health.body.at("slots") == 3);
    const test::HttpJson unknown = test::JsonGet(client, "/nope");
    CHECK(unknown.status == 404);
    CHECK(unknown.body.at("api_version") == "1.0.0");
    CHECK(unknown.body.at("error").at("status") == 404);
}

TEST_CASE("The conversation fixture plays over HTTP", "[hosted-server][http][fixtures]")
{
    ReplayOverHttp("conversation.json");
}

TEST_CASE("The radiant fixture plays over HTTP", "[hosted-server][http][fixtures]")
{
    ReplayOverHttp("radiant.json");
}

TEST_CASE("Each invalid request fixture is refused over HTTP with its status and field, with no model call",
          "[hosted-server][http][fixtures]")
{
    test::HttpHarness harness("http_fixture_invalid");
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/fixture/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture("invalid_requests.json");
    const nlohmann::json start = {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}};
    const std::string open = test::JsonPost(client, "/v1/runs/fixture/conversations", start).body.at("conversation_id").get<std::string>();
    const std::string ended = test::JsonPost(client, "/v1/runs/fixture/conversations", start).body.at("conversation_id").get<std::string>();
    REQUIRE(test::JsonPost(client, "/v1/runs/fixture/conversations/" + ended + "/end", nlohmann::json::object()).status ==
            200);

    for (const nlohmann::json& entry : fixture.at("requests")) {
        const std::string name = entry.at("name").get<std::string>();
        INFO(name);
        const std::string named = entry.at("conversation").get<std::string>();
        const std::string id = named == "open" ? open : named == "ended" ? ended : named;
        const test::HttpTurn turn =
            test::PostTurn(client, "/v1/runs/fixture/conversations/" + id + "/turn", TurnBody(entry.at("turn")));
        // An unknown conversation is a 404 over HTTP; every other fault is the request's (400).
        CHECK(turn.status == (name == "unknown conversation" ? 404 : 400));
        CHECK(turn.events.empty());
        CHECK(turn.body.at("api_version") == "1.0.0");
        CHECK_THAT(turn.body.at("error").at("message").get<std::string>(),
                   ContainsSubstring(entry.at("error").get<std::string>()));
    }
    CHECK(harness.chat.RequestsTo("/completion").empty());
}

TEST_CASE("A client that leaves mid-line stops the turn: nothing is committed, and the next turn works",
          "[hosted-server][http]")
{
    test::HttpHarness harness("http_disconnect");
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/r/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(test::JsonPost(client, "/v1/runs/r/conversations",
                           {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    const nlohmann::json body = {{"speaker", test::HULDA}, {"player_text", "Hello."}, {"observation", test::FixtureObservation()}};

    // The line streams two pieces, then the fake holds the stream open until its client closes it.
    harness.chat.QueueCompletion({test::Pieces(NONE)});
    test::FakeLlamaServer::CompletionScript held;
    held.pieces = {"Well, ", "let me "};
    held.holdOpen = true;
    harness.chat.QueueCompletion(held);
    const test::HttpTurn left = test::PostTurn(client, "/v1/runs/r/conversations/c1/turn", body,
                                               [](const server::StreamEvent& event) { return event.name != "token"; });
    REQUIRE_FALSE(left.events.empty());
    CHECK(left.events.back().name == "token");

    // The server sees the client gone and stops the turn, which closes the held completion stream.
    CHECK(test::WaitUntil([&] { return harness.chat.ClientClosedStream(); }));

    // Nothing was committed: the next turn is turn 1.
    harness.chat.QueueCompletion({test::Pieces(NONE)});
    harness.chat.QueueCompletion({test::Pieces("Fine.")});
    httplib::Client again = harness.Client();
    const test::HttpTurn next = test::PostTurn(again, "/v1/runs/r/conversations/c1/turn", body);
    REQUIRE(next.status == 200);
    REQUIRE_FALSE(next.events.empty());
    CHECK(next.events.back().name == "done");
    CHECK(next.events.back().data.at("turn") == 1);
}

TEST_CASE("Two runs play at once, each conversation on its own slot, and folding uses the upkeep slot",
          "[hosted-server][http]")
{
    test::HttpHarness harness("http_two_runs");
    for (const char* runId : {"a", "b"}) {
        httplib::Client client = harness.Client();
        REQUIRE(test::JsonPost(client, std::string("/v1/runs/") + runId + "/open", test::ReadFixture("open.json")).status ==
                200);
        REQUIRE(test::JsonPost(client, std::string("/v1/runs/") + runId + "/conversations",
                               {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                    .status == 200);
    }

    // Every completion gets the same reply, so it does not matter which run the fake answers first.
    for (int i = 0; i < 4; ++i) {
        harness.chat.QueueCompletion({test::Pieces(NONE)});
    }
    std::vector<test::HttpTurn> turns(2);
    {
        std::vector<std::jthread> players;
        for (std::size_t i = 0; i < 2; ++i) {
            players.emplace_back([&harness, &turns, i] {
                httplib::Client client = harness.Client();
                turns[i] = test::PostTurn(client, std::string("/v1/runs/") + (i == 0 ? "a" : "b") + "/conversations/c1/turn",
                                          {{"speaker", test::HULDA}, {"player_text", "Hi."}, {"observation", test::FixtureObservation()}});
            });
        }
    }
    for (const test::HttpTurn& turn : turns) {
        REQUIRE(turn.status == 200);
        REQUIRE_FALSE(turn.events.empty());
        CHECK(turn.events.back().name == "done");
    }

    // Each conversation's two calls carried one slot, and the two conversations had different ones.
    std::vector<int> slots;
    for (const test::FakeLlamaServer::Request& request : harness.chat.RequestsTo("/completion")) {
        slots.push_back(request.body.at("id_slot").get<int>());
    }
    std::sort(slots.begin(), slots.end());
    CHECK(slots == std::vector<int>{1, 1, 2, 2});

    // Run a's conversation ends: its turn is folded in the background, on the upkeep slot.
    harness.chat.QueueCompletion({test::Pieces("They met at the inn.")});
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/a/conversations/c1/end", nlohmann::json::object()).status == 200);
    CHECK(test::WaitUntil([&] {
        return test::JsonGet(client, "/v1/runs/a/conversations/c1").body.at("conversation").at("folded_through") == 1;
    }));
    const std::vector<test::FakeLlamaServer::Request> completions = harness.chat.RequestsTo("/completion");
    REQUIRE(completions.size() == 5);
    CHECK(completions.back().body.at("id_slot") == server::SlotPool::UPKEEP_SLOT);
}

TEST_CASE("HttpServer refuses a port another server listens on", "[hosted-server][http]")
{
    test::HttpHarness harness("http_port");
    server::HttpServer second(harness.api);
    CHECK_THROWS_AS(second.Start(harness.http.Port()), core::DataError);
    CHECK_THROWS_WITH(second.Start(harness.http.Port()), ContainsSubstring(std::to_string(harness.http.Port())));
    // The first still answers.
    httplib::Client client = harness.Client();
    CHECK(test::JsonGet(client, "/health").status == 200);
}
```

Notes for the implementer:
- `QueueCompletion({test::Pieces(...)})` builds a `CompletionScript` whose first member, `pieces`, is the list.
- Watch for nlohmann's initializer-list ambiguity: `{test::HULDA, test::AELA}` is an array of two strings here because it sits in a `{"participants", ...}` pair; if a body comes out as an object where an array was meant, write `nlohmann::json::array({...})`.
- If `CHECK(... .at("folded_through") == 1)` inside `WaitUntil` fails because the GET's body is null (a failed request), let the lambda return false in that case.

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/HttpServer.h'`.

- [ ] **Step 4: Write `HttpServer`**

Create `src/server/HttpServer.h`:

```cpp
#pragma once

#include <memory>
#include <thread>

#include "server/Api.h"

namespace httplib {
class Server; // cpp-httplib, included only by HttpServer.cpp.
}

namespace morpheus::server {

// morpheus_server's HTTP side (spec, sections 1 and 4) on cpp-httplib: every request goes to Api::Handle, whatever
// its method and path. A JSON answer is written as it is; a turn's stream as Server-Sent Events, one "event: <name>"
// and one "data: <JSON>" line each, written as they come. A client that disconnects during a stream (or the server
// stopping under it) cancels its turn. Listens on 127.0.0.1 only, on a port it owns alone.
class HttpServer {
private:
    Api* _pApi = nullptr;                                 // Not owned; outlives this.
    std::unique_ptr<httplib::Server> _pServer = nullptr; // Set by the constructor.
    int _port = 0;                                        // 0 until Start succeeds.
    std::jthread _listener = {};                          // Last: joined first on destruction.

public:
    explicit HttpServer(Api& api);
    // Stop.
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds port `port` of 127.0.0.1 (0 picks a free port), alone, and serves on a thread of its own. Throws
    // DataError naming the port when it cannot be bound (another server listens on it, ours or not), and
    // ContractError when already started or for a port out of range.
    void Start(int port);

    // The port it listens on; 0 before Start.
    int Port() const;

    // Stops accepting and ends open streams (their turns are cancelled), then waits for the handlers. Callers stop the
    // runs first (RunRegistry::StopAll), so no handler waits on a turn. Safe to call twice.
    void Stop();
};

} // namespace morpheus::server
```

Create `src/server/HttpServer.cpp`:

```cpp
#include "server/HttpServer.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

#include "core/Errors.h"

namespace morpheus::server {

namespace {

constexpr const char* HOST = "127.0.0.1";
constexpr int MAX_PORT = 65535;
// How long a stream waits for its turn's next event before it lets cpp-httplib check the client again.
constexpr std::chrono::milliseconds STREAM_POLL = std::chrono::milliseconds(50);

} // namespace

HttpServer::HttpServer(Api& api)
    : _pApi(&api)
    , _pServer(std::make_unique<httplib::Server>())
{
}

HttpServer::~HttpServer()
{
    Stop();
}

void HttpServer::Start(int port)
{
    MORPHEUS_REQUIRE(_port == 0, "HttpServer::Start: the server is already started");
    MORPHEUS_REQUIRE(port >= 0 && port <= MAX_PORT, std::format("HttpServer::Start: port {} is out of range", port));

    // Every request goes to the Api, whatever its method: it answers 404 for what it does not know.
    auto handle = [this](const httplib::Request& request, httplib::Response& response) {
        ApiResponse answer = _pApi->Handle(request.method, request.path, request.body);
        response.status = answer.status;
        if (!answer.stream) {
            response.set_content(answer.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                                 "application/json");
            return;
        }
        // A turn: its events as Server-Sent Events, as they come. Between events the provider returns without
        // writing, so cpp-httplib checks the client is still there before calling it again. When the client is gone
        // or the server stops, the stream ends unsuccessfully and the releaser cancels the turn.
        std::shared_ptr<TurnStream> stream = std::move(answer.stream);
        response.set_header("Cache-Control", "no-cache");
        response.set_chunked_content_provider(
            "text/event-stream",
            [stream](std::size_t, httplib::DataSink& sink) {
                if (std::optional<StreamEvent> event = stream->Next(STREAM_POLL)) {
                    const std::string text =
                        "event: " + event->name + "\ndata: " +
                        event->data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n\n";
                    return sink.write(text.data(), text.size());
                }
                if (stream->Ended()) {
                    sink.done();
                }
                return true;
            },
            [stream](bool success) {
                if (!success) {
                    stream->Cancel();
                }
            });
    };
    _pServer->Get(".*", handle);
    _pServer->Post(".*", handle);
    _pServer->Put(".*", handle);
    _pServer->Patch(".*", handle);
    _pServer->Delete(".*", handle);

    // The listening socket owns its port alone. cpp-httplib's default (SO_REUSEADDR on Windows, SO_REUSEPORT
    // elsewhere) lets a second server bind a port another one still listens on, and connections then split between
    // them; that must fail instead.
    _pServer->set_socket_options([](socket_t sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    int bound = -1;
    if (port == 0) {
        bound = _pServer->bind_to_any_port(HOST);
    } else if (_pServer->bind_to_port(HOST, port)) {
        bound = port;
    }
    if (bound <= 0) {
        throw core::DataError(
            std::format("port {} of {} cannot be bound: another server may be listening on it", port, HOST));
    }
    _port = bound;
    _listener = std::jthread([this] { _pServer->listen_after_bind(); });
    _pServer->wait_until_ready();
}

int HttpServer::Port() const
{
    return _port;
}

void HttpServer::Stop()
{
    _pServer->stop();
    if (_listener.joinable()) {
        _listener.join();
    }
}

} // namespace morpheus::server
```

If a failed `bind_to_port` leaves the `httplib::Server` unable to bind again, `HttpServer::Start` may be called only once per object; the port test calls it twice on `second`, so in that case make `Start` replace `_pServer` with a fresh `httplib::Server` before binding.

Add `HttpServer.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[http]"`
Expected: `All tests passed`. Then the whole server: `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"` passes, three runs in a row.

- [ ] **Step 6: Commit**

```bash
git add src/server/HttpServer.h src/server/HttpServer.cpp src/server/CMakeLists.txt tests/server/ServerTestSupport.h tests/server/HttpServerTests.cpp
git commit -m "HttpServer: the Api over HTTP with Server-Sent Events, stop on disconnect, an exclusive port; the fixtures replayed over HTTP"
```

---

