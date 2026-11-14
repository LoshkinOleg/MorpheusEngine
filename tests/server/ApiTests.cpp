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

TEST_CASE("The /health route reports the models, the slots and the API version", "[hosted-server][api]")
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

TEST_CASE("The /v1/tokenize and /v1/embeddings routes answer with the utility backend", "[hosted-server][api]")
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
