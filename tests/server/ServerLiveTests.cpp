#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "hosted/HostedTestSupport.h"
#include "llamaserver/LiveServer.h"
#include "llamaserver/LlamaServerBackend.h"
#include "server/Api.h"
#include "server/HttpServer.h"
#include "server/RunRegistry.h"
#include "server/ServerTestSupport.h"
#include "server/SlotPool.h"

using namespace morpheus;

TEST_CASE("morpheus_server against real llama-servers: the conversation fixture over HTTP, and the time to each "
          "turn's first sentence",
          "[server]")
{
    // The owner's servers (README, "The [server] tests"); skipped when the variables are unset.
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(server::SlotPool::UPKEEP_SLOT);
    llamaserver::LlamaServerBackend utility(config);
    server::SlotPool slots(utility.SlotCount());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = test::LIVE_CONTEXT_SIZE;
    server::RunRegistry runs(test::FreshTempDir("server_live") / "runs", settings,
                             [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); },
                             [](const std::string&) {});
    server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                    [](const std::string&) {});
    server::HttpServer http(api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    client.set_read_timeout(std::chrono::seconds(600));

    REQUIRE(test::JsonPost(client, "/v1/runs/live/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture("conversation.json");
    const test::HttpJson started =
        test::JsonPost(client, "/v1/runs/live/conversations",
                       {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}});
    REQUIRE(started.status == 200);
    const std::string base = "/v1/runs/live/conversations/" + started.body.at("conversation_id").get<std::string>();

    // The fixture's requests, answered by the real model. The first turn fills the slot's cache; later turns show the
    // warm figure the host's 1.5 s target is about (measured here, not asserted: the target is for its RTX 3060 Ti).
    int turnNumber = 0;
    const nlohmann::json& steps = fixture.at("steps");
    std::size_t lastTurnStep = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].contains("turn")) {
            lastTurnStep = i;
        }
    }
    for (std::size_t stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
        const nlohmann::json& step = steps[stepIndex];
        if (step.contains("events")) {
            REQUIRE(test::JsonPost(client, base + "/events", {{"events", step.at("events")}}).status == 200);
            continue;
        }
        nlohmann::json body = step.at("turn");
        // The fixture's last turn asks for the review: it must still complete and end with done.
        if (stepIndex == lastTurnStep) {
            body["review"] = true;
        }
        if (!body.contains("observation")) {
            body["observation"] = test::FixtureObservation();
        }
        const auto sent = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::duration> firstSentence;
        const test::HttpTurn turn = test::PostTurn(client, base + "/turn", body, [&](const server::StreamEvent& event) {
            if (event.name == "sentence" && !firstSentence) {
                firstSentence = std::chrono::steady_clock::now() - sent;
            }
            return true;
        });
        ++turnNumber;
        INFO("turn " << turnNumber);
        REQUIRE(turn.status == 200);
        REQUIRE_FALSE(turn.events.empty());
        CHECK(turn.events.front().name == "action");
        CHECK(turn.events.back().name == "done");
        const nlohmann::json& done = turn.events.back().data;
        WARN(std::format("turn {}: first sentence after {} ms; action {}; line: {}", turnNumber,
                         firstSentence ? std::chrono::duration_cast<std::chrono::milliseconds>(*firstSentence).count() : -1,
                         done.at("action").dump(), done.at("line").get<std::string>()));
    }
    runs.StopAll();
    http.Stop();
}

TEST_CASE("morpheus_server against real llama-servers: the decide fixture over HTTP chooses valid speakers",
          "[server][decide]")
{
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(server::SlotPool::UPKEEP_SLOT);
    llamaserver::LlamaServerBackend utility(config);
    server::SlotPool slots(utility.SlotCount());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = test::LIVE_CONTEXT_SIZE;
    server::RunRegistry runs(test::FreshTempDir("server_live_decide") / "runs", settings,
                             [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); },
                             [](const std::string&) {});
    server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                    [](const std::string&) {});
    server::HttpServer http(api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    client.set_read_timeout(std::chrono::seconds(600));

    REQUIRE(test::JsonPost(client, "/v1/runs/live/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture("decide.json");
    const test::HttpJson started =
        test::JsonPost(client, "/v1/runs/live/conversations",
                       {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}});
    REQUIRE(started.status == 200);
    const std::string base = "/v1/runs/live/conversations/" + started.body.at("conversation_id").get<std::string>();

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

TEST_CASE("morpheus_server against real llama-servers: nobody remembers what it did not witness, over HTTP",
          "[server][memory]")
{
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(server::SlotPool::UPKEEP_SLOT);
    llamaserver::LlamaServerBackend utility(config);
    server::SlotPool slots(utility.SlotCount());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = test::LIVE_CONTEXT_SIZE;
    server::RunRegistry runs(test::FreshTempDir("server_live_memory") / "runs", settings,
                             [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); },
                             [](const std::string&) {});
    server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                    [](const std::string&) {});
    server::HttpServer http(api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    client.set_read_timeout(std::chrono::seconds(600));
    // The model writes the lines and the memories: only what cannot depend on its words is checked, and a memories
    // check waits up to 5 minutes for the background fold.
    test::PlayMemoryFixtureOverHttp(client, "live_memory", test::ReadFixture("three_actors.json"), false,
                                    std::chrono::minutes(5));
    runs.StopAll();
    http.Stop();
}

TEST_CASE("morpheus_server against real llama-servers: the reload fixture, over HTTP", "[server][rewind]")
{
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(server::SlotPool::UPKEEP_SLOT);
    llamaserver::LlamaServerBackend utility(config);
    server::SlotPool slots(utility.SlotCount());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = test::LIVE_CONTEXT_SIZE;
    server::RunRegistry runs(test::FreshTempDir("server_live_rewind") / "runs", settings,
                             [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); },
                             [](const std::string&) {});
    server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                    [](const std::string&) {});
    server::HttpServer http(api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    client.set_read_timeout(std::chrono::seconds(600));
    // Only what cannot depend on the model's words is checked; a memories check waits up to 5 minutes for the fold.
    test::PlayMemoryFixtureOverHttp(client, "live_rewind", test::ReadFixture("reload.json"), false,
                                    std::chrono::minutes(5));
    runs.StopAll();
    http.Stop();
}
