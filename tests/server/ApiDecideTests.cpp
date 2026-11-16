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
