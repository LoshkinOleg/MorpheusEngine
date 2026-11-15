#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"
#include "turn/EngineText.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string FOLLOW = R"({"action":"follow","params":{}})";
const std::string NONE = R"({"action":"none","params":{}})";

// Opens run save_1 and starts conversation c1 of Hulda and Aela in `harness`.
void OpenWithConversation(test::ApiHarness& harness)
{
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness
                .Post("/v1/runs/save_1/conversations",
                      {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
}

// A turn request body: `speaker` speaks after `playerText` in observation.json.
nlohmann::json TurnBody(const std::string& speaker, const std::string& playerText)
{
    return {{"speaker", speaker}, {"player_text", playerText}, {"observation", test::FixtureObservation()}};
}

const std::string TURN = "/v1/runs/save_1/conversations/c1/turn";

} // namespace

TEST_CASE("A turn streams its action, its tokens and its sentences, then done, and commits", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {FOLLOW};
    script.lines = {"Lead the way, then. I will follow you."};
    test::ApiHarness harness("api_turn", script.Responder());
    OpenWithConversation(harness);

    const server::ApiResponse response = harness.Post(TURN, TurnBody(test::HULDA, "Come with me."));
    REQUIRE(response.status == 200);
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    REQUIRE(events.size() >= 4);

    // The action first, with the API version; done last.
    CHECK(events.front().name == "action");
    CHECK(events.front().data == nlohmann::json{{"api_version", "1.0.0"},
                                                {"speaker", test::HULDA},
                                                {"action", {{"id", "follow"}, {"params", nlohmann::json::object()}}}});
    CHECK(events.back().name == "done");

    // Tokens make the line; each sentence comes as soon as it ends, before the next sentence's tokens.
    std::string tokens;
    std::string tokensBeforeFirstSentence;
    std::vector<std::string> sentences;
    for (std::size_t i = 1; i + 1 < events.size(); ++i) {
        if (events[i].name == "token") {
            tokens += events[i].data.at("text").get<std::string>();
        } else {
            REQUIRE(events[i].name == "sentence");
            if (sentences.empty()) {
                tokensBeforeFirstSentence = tokens;
            }
            sentences.push_back(events[i].data.at("text").get<std::string>());
        }
    }
    CHECK(tokens == "Lead the way, then. I will follow you.");
    CHECK(sentences == std::vector<std::string>{"Lead the way, then.", "I will follow you."});
    CHECK_THAT(tokensBeforeFirstSentence, ContainsSubstring("then."));
    CHECK_THAT(tokensBeforeFirstSentence, !ContainsSubstring("follow"));

    const nlohmann::json& done = events.back().data;
    CHECK(done.at("speaker") == test::HULDA);
    CHECK(done.at("action") == nlohmann::json{{"id", "follow"}, {"params", nlohmann::json::object()}});
    CHECK(done.at("line") == "Lead the way, then. I will follow you.");
    CHECK(done.at("turn") == 1);
    CHECK(done.at("events_omitted") == 0);
    CHECK(done.at("timings").contains("resolve"));
    CHECK(done.at("timings").contains("speak"));

    // Committed: the stored turn reads back.
    const server::ApiResponse read = harness.Get("/v1/runs/save_1/conversations/c1");
    REQUIRE(read.body.at("turns").size() == 1);
    CHECK(read.body.at("turns")[0].at("line") == "Lead the way, then. I will follow you.");
}

TEST_CASE("A refused action streams null with its reasons, and the turn goes on", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"fly","params":{}})"};
    script.lines = {"No."};
    test::ApiHarness harness("api_turn_refused", script.Responder());
    OpenWithConversation(harness);

    const server::ApiResponse response = harness.Post(TURN, TurnBody(test::HULDA, "Fly away."));
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    REQUIRE(events.size() >= 2);
    CHECK(events.front().data.at("action").is_null());
    REQUIRE(events.front().data.at("refused").is_array());
    CHECK_FALSE(events.front().data.at("refused").empty());
    CHECK(events.back().name == "done");
    CHECK(events.back().data.at("action").is_null());
    CHECK(events.back().data.at("line") == "No.");
}

TEST_CASE("A turn's errors before its action are JSON with their status, and nothing is committed",
          "[hosted-server][api]")
{
    test::HostedScript script; // No reply: only the backend-failure case below reaches the model, and it throws.
    test::ApiHarness harness("api_turn_errors", [&script](const inference::Prompt& prompt,
                                                          const inference::GenerationSpec& spec) -> std::string {
        if (prompt.messages.back().content.find("Fail now.") != std::string::npos) {
            throw inference::InferenceError("cannot reach http://127.0.0.1:1/completion");
        }
        return script.Responder()(prompt, spec);
    });
    OpenWithConversation(harness);
    // Checks that `response` is the error `status` naming `part`.
    auto check = [](const server::ApiResponse& response, int status, const std::string& part) {
        INFO(response.body.dump());
        CHECK(response.stream == nullptr);
        CHECK(response.status == status);
        CHECK(response.body.at("api_version") == "1.0.0");
        CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
    };

    check(harness.Post(TURN, TurnBody("decide", "Hello.")), 400, "decide");
    check(harness.Post(TURN, {{"speaker", test::HULDA}}), 400, "observation");
    check(harness.Post(TURN, {{"speaker", 7}, {"observation", test::FixtureObservation()}}), 400, "speaker");
    check(harness.Post(TURN, {{"speaker", test::HULDA}, {"player_text", 7}, {"observation", test::FixtureObservation()}}),
          400, "player_text");
    check(harness.Post(TURN, {{"speaker", test::HULDA}, {"allowed_actions", {7}}, {"observation", test::FixtureObservation()}}),
          400, "allowed_actions/0");
    check(harness.Post("/v1/runs/save_1/conversations/c9/turn", TurnBody(test::HULDA, "Hello.")), 404, "c9");
    check(harness.Post("/v1/runs/save_9/conversations/c1/turn", TurnBody(test::HULDA, "Hello.")), 409, "save_9");
    check(harness.Post(TURN, TurnBody(test::AELA, "Hello.")), 400, "is the player");
    check(harness.Post(TURN, TurnBody(test::HULDA, std::string(5000, 'x'))), 422, "tail_allowance");
    check(harness.Post(TURN, TurnBody(test::HULDA, "Fail now.")), 503, "cannot reach");

    CHECK(harness.Get("/v1/runs/save_1/conversations/c1").body.at("turns").empty());
}

TEST_CASE("A turn's calls use its conversation's slot, and ending the conversation frees it", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"Welcome.", "Hello."};
    test::ApiHarness harness("api_turn_slots", script.Responder());
    OpenWithConversation(harness);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .body.at("conversation_id") == "c2");

    test::ReadAll(*harness.Post(TURN, TurnBody(test::HULDA, "Hi.")).stream);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c2/turn", TurnBody(test::HULDA, "Hi.")).stream);

    // The backend is the run's: read its calls on the run's worker.
    std::vector<int> slots;
    harness.runs.Find("save_1")->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
        for (const test::ScriptedBackend::Call& call : harness.backends.at(0)->Calls()) {
            slots.push_back(call.spec.slot);
        }
    });
    CHECK(slots == std::vector<int>{1, 1, 2, 2});
    CHECK(harness.slots.HolderOf(1) == "save_1/c1");

    REQUIRE(harness.Post("/v1/runs/save_1/conversations/c1/end", nlohmann::json::object()).status == 200);
    CHECK(harness.slots.HolderOf(1) == "");
    CHECK(harness.slots.HolderOf(2) == "save_1/c2");
}

TEST_CASE("A turn whose stream is cancelled stops, commits nothing, and the next turn works", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"A long answer that streams slowly, piece by piece, for a while.", "Fine."};
    test::ApiHarness harness("api_turn_cancel", script.Responder());
    harness.pieceDelay = std::chrono::milliseconds(100);
    OpenWithConversation(harness);

    const server::ApiResponse response = harness.Post(TURN, TurnBody(test::HULDA, "Hello."));
    REQUIRE(response.stream != nullptr);
    // Read up to the first token, then leave.
    bool sawToken = false;
    for (int i = 0; i < 100 && !sawToken; ++i) {
        if (std::optional<server::StreamEvent> event = response.stream->Next(std::chrono::milliseconds(100))) {
            sawToken = event->name == "token";
        }
    }
    REQUIRE(sawToken);
    response.stream->Cancel();
    const std::vector<server::StreamEvent> rest = test::ReadAll(*response.stream);
    for (const server::StreamEvent& event : rest) {
        CHECK(event.name != "done");
    }

    // Nothing was committed: the next turn is turn 1.
    const server::ApiResponse next = harness.Post(TURN, TurnBody(test::HULDA, "Still there?"));
    REQUIRE(next.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*next.stream);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().name == "done");
    CHECK(events.back().data.at("turn") == 1);
}

namespace {

// A responder for background-folding tests: the script's replies, and for each fold call (counted in `foldCalls`) a
// summary of `summaryBytes` bytes, which streams in pieces of 4 bytes.
test::ScriptedBackend::Responder SlowFoldResponder(test::HostedScript& script, std::atomic<int>& foldCalls,
                                                   std::size_t summaryBytes)
{
    return [&script, &foldCalls, summaryBytes](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (test::IsHostedFold(prompt)) {
            ++foldCalls;
            return std::string(summaryBytes, 's');
        }
        return script.Responder()(prompt, spec);
    };
}

// Conversation c1's folded_through, through GET.
int FoldedThrough(test::ApiHarness& harness)
{
    return harness.Get("/v1/runs/save_1/conversations/c1").body.at("conversation").at("folded_through").get<int>();
}

} // namespace

TEST_CASE("Requests other than turns wait for a background fold, which lands while a client keeps polling",
          "[hosted-server][api]")
{
    // Each piece streams after 25 ms: the fold's summary of 40 pieces takes about a second.
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Welcome."};
    std::atomic<int> foldCalls = 0;
    test::ApiHarness harness("api_fold_poll", SlowFoldResponder(script, foldCalls, 160));
    harness.pieceDelay = std::chrono::milliseconds(25);
    OpenWithConversation(harness);
    test::ReadAll(*harness.Post(TURN, TurnBody(test::HULDA, "Hi.")).stream);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations/c1/end", nlohmann::json::object()).status == 200);

    // The client polls without pause; each GET waits for the fold's pass instead of starting it over.
    bool folded = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!folded && std::chrono::steady_clock::now() < deadline) {
        folded = FoldedThrough(harness) == 1;
    }
    CHECK(folded);
    CHECK(foldCalls.load() == 1);
}

TEST_CASE("A turn during a background fold stops it and runs promptly; the fold lands afterwards",
          "[hosted-server][api]")
{
    // Each piece streams after 25 ms: the fold's summary of 100 pieces takes about 2.5 s, a turn about 250 ms.
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"Welcome.", "Hello."};
    std::atomic<int> foldCalls = 0;
    test::ApiHarness harness("api_fold_turn", SlowFoldResponder(script, foldCalls, 400));
    harness.pieceDelay = std::chrono::milliseconds(25);
    OpenWithConversation(harness);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    test::ReadAll(*harness.Post(TURN, TurnBody(test::HULDA, "Hi.")).stream);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations/c1/end", nlohmann::json::object()).status == 200);

    // While c1 folds, c2's turn arrives: it stops the fold and plays at once.
    REQUIRE(test::WaitUntil([&] { return foldCalls.load() >= 1; }));
    const auto start = std::chrono::steady_clock::now();
    const server::ApiResponse response = harness.Post("/v1/runs/save_1/conversations/c2/turn", TurnBody(test::HULDA, "Hi."));
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().name == "done");

    // The fold runs again afterwards and lands (the GET waits for it).
    CHECK(test::WaitUntil([&] { return FoldedThrough(harness) == 1; }));
    CHECK(foldCalls.load() >= 2);
}

TEST_CASE("A turn stopped by shutdown mid-line ends its stream with a 503 error, and commits nothing",
          "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"A long answer that streams slowly, piece by piece, for a while."};
    test::ApiHarness harness("api_turn_shutdown", script.Responder());
    harness.pieceDelay = std::chrono::milliseconds(100);
    OpenWithConversation(harness);

    const server::ApiResponse response = harness.Post(TURN, TurnBody(test::HULDA, "Hello."));
    REQUIRE(response.stream != nullptr);
    // Read up to the first token, then the server stops.
    bool sawToken = false;
    for (int i = 0; i < 100 && !sawToken; ++i) {
        if (std::optional<server::StreamEvent> event = response.stream->Next(std::chrono::milliseconds(100))) {
            sawToken = event->name == "token";
        }
    }
    REQUIRE(sawToken);
    harness.runs.StopAll();

    // The stream ends with the error, and no done.
    const std::vector<server::StreamEvent> rest = test::ReadAll(*response.stream);
    CHECK(response.stream->Ended());
    REQUIRE_FALSE(rest.empty());
    for (const server::StreamEvent& event : rest) {
        CHECK(event.name != "done");
    }
    CHECK(rest.back().name == "error");
    CHECK(rest.back().data.at("status") == 503);
    CHECK_THAT(rest.back().data.at("message").get<std::string>(), ContainsSubstring("the server is stopping"));
}
