#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"
#include "turn/PromptPack.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The defaults route answers every default text and the variables it takes", "[hosted-server][api][prompts]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_prompts_default", script.Responder());
    const server::ApiResponse response = harness.Get("/v1/prompts/default");
    CHECK(response.status == 200);
    CHECK(response.body.at("api_version") == "1.4.0");
    const turn::PromptPack defaults;
    const nlohmann::json& prompts = response.body.at("prompts");
    CHECK(prompts.size() == 8);
    for (const turn::PromptText& text : turn::PROMPT_TEXTS) {
        INFO(std::string(text.key));
        CHECK(prompts.at(std::string(text.key)) == defaults.Text(text.key));
    }
    CHECK(response.body.at("variables") == nlohmann::json{{"system", nlohmann::json::array()},
                                                          {"resolve", {"speaker", "player"}},
                                                          {"speak", {"speaker", "player"}},
                                                          {"turn_cue", {"speaker", "player"}},
                                                          {"decide", {"player"}},
                                                          {"summarize", {"actor"}},
                                                          {"condense", {"actor"}},
                                                          {"review", nlohmann::json::array()}});
}

TEST_CASE("Open takes a pack: the next turn's prompt shows it; a bad pack is a 400 naming the text and the variable",
          "[hosted-server][api][prompts]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})"};
    script.lines = {"Welcome."};
    test::ApiHarness harness("api_prompts_open", script.Responder());

    nlohmann::json bad = test::ReadFixture("open.json");
    bad["prompts"] = {{"speak", "Say it to {location}."}};
    const server::ApiResponse refused = harness.Post("/v1/runs/save_1/open", bad);
    CHECK(refused.status == 400);
    CHECK_THAT(refused.body.at("error").at("message").get<std::string>(),
               ContainsSubstring("prompts/speak: unknown variable {location}; this text takes {speaker}, {player}"));

    nlohmann::json open = test::ReadFixture("open.json");
    open["prompts"] = {{"system", "# Pack\nYou voice a test scene."}, {"resolve", "PACK: {speaker} acts."}};
    REQUIRE(harness.Post("/v1/runs/save_1/open", open).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                {{"speaker", test::HULDA}, {"player_text", "Hi."}, {"observation", test::FixtureObservation()}})
                       .stream);
    // The run's backend is the last one made (the refused open may have made one before its host data failed).
    REQUIRE_FALSE(harness.backends.empty());
    const std::vector<test::ScriptedBackend::Call>& calls = harness.backends.back()->Calls();
    REQUIRE_FALSE(calls.empty());
    CHECK(calls[0].prompt.messages[0].content.starts_with("# Pack\nYou voice a test scene."));
    CHECK(calls[0].prompt.messages.back().content.ends_with("# Task\nPACK: Hulda acts."));
}
