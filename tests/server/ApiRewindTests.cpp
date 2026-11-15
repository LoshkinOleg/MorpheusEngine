#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The rewind route answers what it hid and the open rollback", "[hosted-server][api][rewind]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})", R"({"action":"none","params":{}})"};
    script.lines = {"One.", "Two."};
    test::ApiHarness harness("api_rewind", test::MemoryEchoResponder(script));
    nlohmann::json open = test::ReadFixture("open.json");
    open["clock"] = "/game_time/days_passed";
    REQUIRE(harness.Post("/v1/runs/save_1/open", open).status == 200);
    nlohmann::json at1 = test::FixtureObservation();
    at1["game_time"]["days_passed"] = 1.0;
    nlohmann::json at2 = at1;
    at2["game_time"]["days_passed"] = 2.0;
    REQUIRE(harness.Post("/v1/runs/save_1/conversations", {{"participants", {test::HULDA, test::AELA}}, {"observation", at1}})
                .status == 200);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                {{"speaker", test::HULDA}, {"player_text", "Hi."}, {"observation", at1}})
                       .stream);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                {{"speaker", test::HULDA}, {"player_text", "Again."}, {"observation", at2}})
                       .stream);

    const server::ApiResponse back = harness.Post("/v1/runs/save_1/rewind", {{"game_time", 1.5}});
    CHECK(back.status == 200);
    CHECK(back.body == nlohmann::json{{"rewound", true},
                                      {"hidden_turns", 1},
                                      {"restored_turns", 0},
                                      {"rollback", {{"game_time", 1.5}}},
                                      {"api_version", "1.3.0"}});
    const server::ApiResponse forward = harness.Post("/v1/runs/save_1/rewind", {{"game_time", 5.0}});
    CHECK(forward.body.at("restored_turns") == 1);
    CHECK(forward.body.at("rollback").is_null());
}

TEST_CASE("The rewind route's errors", "[hosted-server][api][rewind]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_rewind_errors", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    auto check = [](const server::ApiResponse& response, int status, const std::string& part) {
        INFO(response.body.dump());
        CHECK(response.status == status);
        CHECK(response.body.at("api_version") == "1.3.0");
        CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
    };
    check(harness.Post("/v1/runs/save_1/rewind", {{"game_time", 1.0}}), 400, "no clock");
    check(harness.Post("/v1/runs/save_1/rewind", {{"game_time", "noon"}}), 400, "game_time");
    check(harness.Post("/v1/runs/save_1/rewind", nlohmann::json::object()), 400, "game_time");
    check(harness.Post("/v1/runs/save_9/rewind", {{"game_time", 1.0}}), 409, "save_9");
}
