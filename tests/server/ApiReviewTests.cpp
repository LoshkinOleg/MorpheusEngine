#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;

TEST_CASE("A review turn streams the action, then the line after the review, then done with timings.review",
          "[hosted-server][api][review]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})"};
    script.lines = {"Stay close."};
    script.reviews = {R"({"consistent":true,"problems":[]})"};
    test::ApiHarness harness("api_review", script.Responder());
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    const server::ApiResponse response = harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                                      {{"speaker", test::HULDA},
                                                       {"player_text", "Wait here."},
                                                       {"review", true},
                                                       {"observation", test::FixtureObservation()}});
    REQUIRE(response.status == 200);
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    REQUIRE(events.size() >= 3);
    CHECK(events.front().name == "action");
    CHECK(events.front().data.at("api_version") == "1.5.0");

    // The line is held back until the review is done and then delivered in one piece (an unreviewed turn streams it
    // in several): exactly one token event, whose text is the whole line, after the action.
    std::vector<std::string> tokens;
    for (const server::StreamEvent& event : events) {
        if (event.name == "token") {
            tokens.push_back(event.data.at("text").get<std::string>());
        }
    }
    REQUIRE(tokens.size() == 1);
    CHECK(tokens.front() == "Stay close.");
    CHECK(events.back().name == "done");
    const nlohmann::json& timings = events.back().data.at("timings");
    CHECK(timings.contains("review"));
    CHECK(timings.at("review").at("consistent") == true);
    CHECK_FALSE(timings.contains("respeak"));
}

TEST_CASE("A review that finds problems makes done carry timings.respeak and the tokens say the second line",
          "[hosted-server][api][review]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})"};
    script.lines = {"Hulda: Stay close.", "Stay close."};
    script.reviews = {R"({"consistent":false,"problems":["x"]})"};
    test::ApiHarness harness("api_review_respeak", script.Responder());
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    const server::ApiResponse response = harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                                      {{"speaker", test::HULDA},
                                                       {"player_text", "Wait here."},
                                                       {"review", true},
                                                       {"observation", test::FixtureObservation()}});
    REQUIRE(response.status == 200);
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    REQUIRE(events.size() >= 3);
    CHECK(events.front().name == "action");

    // The tokens spell the second line, not the first.
    std::string spoken;
    for (const server::StreamEvent& event : events) {
        if (event.name == "token") {
            spoken += event.data.at("text").get<std::string>();
        }
    }
    CHECK(spoken == "Stay close.");
    CHECK(events.back().name == "done");
    const nlohmann::json& timings = events.back().data.at("timings");
    CHECK(timings.at("review").at("consistent") == false);
    CHECK(timings.contains("respeak"));
}

TEST_CASE("A malformed review is a 400", "[hosted-server][api][review]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_review_bad", script.Responder());
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    const server::ApiResponse response = harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                                      {{"speaker", test::HULDA},
                                                       {"review", "yes"},
                                                       {"observation", test::FixtureObservation()}});
    CHECK(response.status == 400);
    CHECK(response.body.dump().find("review: expected true or false") != std::string::npos);
}
