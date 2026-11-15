#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// Hulda's memories as GET answers them.
nlohmann::json HuldaMemories(test::ApiHarness& harness)
{
    return harness.Get("/v1/runs/save_1/actors/" + test::HULDA + "/memories").body.at("memories");
}

} // namespace

TEST_CASE("The memories of an actor read back, and a share copies them, told by the teller", "[hosted-server][api][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"The well is poisoned."};
    test::ApiHarness harness("api_memory_share", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    test::ReadAll(*harness
                       .Post("/v1/runs/save_1/conversations/c1/turn",
                             {{"speaker", test::HULDA}, {"player_text", "News?"}, {"observation", test::FixtureObservation()}})
                       .stream);

    // A share while the conversation is open waits.
    const server::ApiResponse early = harness.Post("/v1/runs/save_1/memories/share",
                                                   {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}});
    CHECK(early.status == 200);
    CHECK(early.body == nlohmann::json{{"shared", 0}, {"pending", true}, {"api_version", "1.3.0"}});

    // Ended, the conversation is folded in the background; the pending share then applies.
    REQUIRE(harness.Post("/v1/runs/save_1/conversations/c1/end", nlohmann::json::object()).status == 200);
    REQUIRE(test::WaitUntil([&] {
        return harness.Get("/v1/runs/save_1/actors/" + test::SAADIA + "/memories").body.at("memories").size() == 1;
    }));
    const nlohmann::json hulda = HuldaMemories(harness);
    REQUIRE(hulda.size() == 1);
    CHECK(hulda[0].at("conversation_id") == "c1");
    CHECK(hulda[0].at("told_by") == "");
    CHECK(hulda[0].at("superseded") == false);
    CHECK_THAT(hulda[0].at("text").get<std::string>(), ContainsSubstring("poisoned"));
    const nlohmann::json saadia = harness.Get("/v1/runs/save_1/actors/" + test::SAADIA + "/memories").body.at("memories");
    CHECK(saadia[0].at("told_by") == test::HULDA);
    CHECK(saadia[0].at("from_turn") == 1);
    CHECK(saadia[0].at("to_turn") == 1);

    // Sharing again copies nothing new.
    const server::ApiResponse again = harness.Post("/v1/runs/save_1/memories/share",
                                                   {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}});
    CHECK(again.body.at("shared") == 0);
    CHECK(again.body.at("pending") == false);
}

TEST_CASE("Memory routes answer their errors with the API version", "[hosted-server][api][memory]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_memory_errors", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    auto check = [](const server::ApiResponse& response, int status, const std::string& part) {
        INFO(response.body.dump());
        CHECK(response.status == status);
        CHECK(response.body.at("api_version") == "1.3.0");
        CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
    };
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c9"}}),
          404, "c9");
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::SAADIA}, {"to", test::HULDA}, {"conversation_id", "c1"}}),
          400, "from");
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::HULDA}, {"conversation_id", "c1"}}), 400, "to");
    check(harness.Post("/v1/runs/save_9/memories/share", {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}}),
          409, "save_9");
    check(harness.Get("/v1/runs/save_9/actors/" + test::HULDA + "/memories"), 409, "save_9");
    // An actor with no memories: an empty list.
    const server::ApiResponse none = harness.Get("/v1/runs/save_1/actors/0000BEEF/memories");
    CHECK(none.status == 200);
    CHECK(none.body.at("memories") == nlohmann::json::array());
}
