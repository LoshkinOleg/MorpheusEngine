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
        CHECK(action.at("api_version") == "1.2.0");
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
    CHECK(health.body.at("api_version") == "1.2.0");
    CHECK(health.body.at("chat_model") == "nomic-embed-text-v1.5.Q8_0"); // The fake names one model file for both.
    CHECK(health.body.at("slots") == 3);
    const test::HttpJson unknown = test::JsonGet(client, "/nope");
    CHECK(unknown.status == 404);
    CHECK(unknown.body.at("api_version") == "1.2.0");
    CHECK(unknown.body.at("error").at("status") == 404);
}

TEST_CASE("Answers cpp-httplib makes itself are JSON with the API version too", "[hosted-server][http]")
{
    test::HttpHarness harness("http_own_answers");
    httplib::Client client = harness.Client();
    // A method no route takes, and a request line cpp-httplib refuses (a method it does not know).
    for (const std::string method : {"OPTIONS", "BREW"}) {
        INFO(method);
        httplib::Request request;
        request.method = method;
        request.path = "/health";
        const httplib::Result result = client.send(request);
        REQUIRE(result);
        CHECK(result->status >= 400);
        CHECK_THAT(result->get_header_value("Content-Type"), StartsWith("application/json"));
        const nlohmann::json body = nlohmann::json::parse(result->body, nullptr, false);
        REQUIRE(body.is_object());
        CHECK(body.at("api_version") == "1.2.0");
        CHECK(body.at("error").at("status") == result->status);
        CHECK_FALSE(body.at("error").at("message").get<std::string>().empty());
    }
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
        CHECK(turn.body.at("api_version") == "1.2.0");
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
    const std::size_t embeddedBefore = harness.embed.RequestsTo("/embedding").size();
    REQUIRE(test::JsonPost(client, "/v1/runs/a/conversations/c1/end", nlohmann::json::object()).status == 200);
    // The test waits on the fakes: the ended conversation's turn is embedded as history (a document) as its background
    // upkeep starts. The GET below then waits for the running fold (only a turn stops it), so Hulda's memory is stored.
    CHECK(test::WaitUntil([&] {
        const std::vector<test::FakeLlamaServer::Request> embedded = harness.embed.RequestsTo("/embedding");
        return std::any_of(embedded.begin() + static_cast<std::ptrdiff_t>(embeddedBefore), embedded.end(),
                           [](const test::FakeLlamaServer::Request& request) {
                               return request.body.at("content").get<std::string>().starts_with("search_document: ");
                           });
    }));
    const test::HttpJson conversation = test::JsonGet(client, "/v1/runs/a/conversations/c1");
    REQUIRE(conversation.status == 200);
    CHECK(conversation.body.at("conversation").at("folded_through") == 1);
    const std::vector<test::FakeLlamaServer::Request> completions = harness.chat.RequestsTo("/completion");
    REQUIRE(completions.size() == 5);
    CHECK(completions.back().body.at("id_slot") == server::SlotPool::UPKEEP_SLOT);
}

TEST_CASE("Opening a run whose lore cannot be embedded: 503 when the embedding server fails, 400 when it refuses the text",
          "[hosted-server][http]")
{
    // Each case opens a new run, whose retrievable lore is embedded at the open.
    struct Case {
        const char* name = "";
        int status = 0;   // The fake embedding server's answer to /embedding; 0: it drops the connection.
        int expected = 0; // The open's status.
    };
    for (const Case& failure : {Case{"answers 500", 500, 503}, Case{"drops the connection", 0, 503},
                                Case{"refuses the text", 400, 400}}) {
        INFO(failure.name);
        test::HttpHarness harness(std::string("http_embed_") + std::to_string(failure.status));
        if (failure.status == 0) {
            harness.embed.DropNextAnswers("/embedding", 4);
        } else {
            harness.embed.SetStatus("/embedding", failure.status);
        }
        httplib::Client client = harness.Client();
        const test::HttpJson opened = test::JsonPost(client, "/v1/runs/r/open", test::ReadFixture("open.json"));
        CHECK(opened.status == failure.expected);
        CHECK(opened.body.at("api_version") == "1.2.0");
        CHECK(opened.body.at("error").at("status") == failure.expected);
        if (failure.expected == 400) {
            CHECK_THAT(opened.body.at("error").at("message").get<std::string>(), ContainsSubstring("cannot be embedded"));
        }
    }
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
