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

TEST_CASE("The done event carries timings.decide only when a decide call ran", "[hosted-server][api][decide]")
{
    test::HostedScript script;
    script.decisions = {R"({"speaker":"0001B07C"})"};
    script.actions = {R"({"action":"none","params":{}})", R"({"action":"none","params":{}})"};
    script.lines = {"Evening.", "Hello."};
    test::ApiHarness harness("api_decide_timings", script.Responder());
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::SAADIA, test::AELA}},
                          {"observation", test::FixtureObservation()}})
                .status == 200);

    // A decided turn: the model chose, so the done event's timings name the call.
    const server::ApiResponse decided =
        harness.Post("/v1/runs/save_1/conversations/c1/turn",
                     {{"speaker", "decide"}, {"player_text", "Evening, both."}, {"observation", test::FixtureObservation()}});
    REQUIRE(decided.status == 200);
    REQUIRE(decided.stream != nullptr);
    const std::vector<server::StreamEvent> decidedEvents = test::ReadAll(*decided.stream);
    REQUIRE_FALSE(decidedEvents.empty());
    REQUIRE(decidedEvents.back().name == "done");
    CHECK(decidedEvents.back().data.at("timings").contains("decide"));

    // A named speaker: no decide call, so no decide timings.
    const server::ApiResponse named =
        harness.Post("/v1/runs/save_1/conversations/c1/turn",
                     {{"speaker", test::HULDA}, {"player_text", "And you?"}, {"observation", test::FixtureObservation()}});
    REQUIRE(named.status == 200);
    REQUIRE(named.stream != nullptr);
    const std::vector<server::StreamEvent> namedEvents = test::ReadAll(*named.stream);
    REQUIRE_FALSE(namedEvents.empty());
    REQUIRE(namedEvents.back().name == "done");
    CHECK_FALSE(namedEvents.back().data.at("timings").contains("decide"));
}
