#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/HttpServer.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The three-actor fixture plays over HTTP", "[hosted-server][http][memory][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("three_actors.json");
    const nlohmann::json& observation = fixture.at("observation");
    test::HostedScript script;
    for (const nlohmann::json& step : fixture.at("steps")) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    test::ApiHarness harness("http_memory_fixture", test::MemoryEchoResponder(script));
    server::HttpServer http(harness.api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    REQUIRE(test::JsonPost(client, "/v1/runs/r/open", test::ReadFixture("open.json")).status == 200);
    auto start = [&](const nlohmann::json& participants) {
        const test::HttpJson started =
            test::JsonPost(client, "/v1/runs/r/conversations", {{"participants", participants}, {"observation", observation}});
        REQUIRE(started.status == 200);
        return started.body.at("conversation_id").get<std::string>();
    };
    std::string id = start(fixture.at("participants"));
    const std::string firstId = id;
    // An actor's memories as the server answers them.
    auto memoriesOf = [&](const std::string& actor) {
        return test::JsonGet(client, "/v1/runs/r/actors/" + actor + "/memories").body.at("memories");
    };

    for (std::size_t i = 0; i < fixture.at("steps").size(); ++i) {
        const nlohmann::json& step = fixture.at("steps")[i];
        INFO("step " << i);
        const std::string base = "/v1/runs/r/conversations/" + id;
        if (step.contains("turn")) {
            nlohmann::json body = step.at("turn");
            body["observation"] = observation;
            const test::HttpTurn turn = test::PostTurn(client, base + "/turn", body);
            REQUIRE(turn.status == 200);
            REQUIRE_FALSE(turn.events.empty());
            CHECK(turn.events.back().name == "done"); // The prompt checks need the session: HostedFixtureTests.
        } else if (step.contains("leave")) {
            CHECK(test::JsonPost(client, base + "/participants", {{"left", step.at("leave")}, {"observation", observation}})
                      .status == 200);
        } else if (step.contains("join")) {
            CHECK(test::JsonPost(client, base + "/participants", {{"joined", step.at("join")}, {"observation", observation}})
                      .status == 200);
        } else if (step.contains("end")) {
            CHECK(test::JsonPost(client, base + "/end", nlohmann::json::object()).status == 200);
        } else if (step.contains("maintain")) {
            // The server folds in the background: the memories checks below wait for what they expect.
        } else if (step.contains("memories")) {
            const nlohmann::json& check = step.at("memories");
            const std::string actor = check.at("actor").get<std::string>();
            // Every memory's text, once the expected ones have arrived.
            auto texts = [&] {
                std::string all;
                for (const nlohmann::json& memory : memoriesOf(actor)) {
                    all += memory.at("text").get<std::string>() + "\n";
                }
                return all;
            };
            const nlohmann::json contains = check.value("contains", nlohmann::json::array());
            CHECK(test::WaitUntil([&] {
                const std::string all = texts();
                for (const nlohmann::json& present : contains) {
                    if (all.find(present.get<std::string>()) == std::string::npos) {
                        return false;
                    }
                }
                return true;
            }));
            const std::string all = texts();
            for (const nlohmann::json& absent : check.value("absent", nlohmann::json::array())) {
                CHECK_THAT(all, !ContainsSubstring(absent.get<std::string>()));
            }
            if (check.contains("count")) {
                CHECK(memoriesOf(actor).size() == check.at("count").get<std::size_t>());
            }
            // Kept in a local: items() of a temporary would dangle.
            const nlohmann::json toldBy = check.value("told_by", nlohmann::json::object());
            for (const auto& item : toldBy.items()) {
                bool found = false;
                for (const nlohmann::json& memory : memoriesOf(actor)) {
                    if (memory.at("text").get<std::string>().find(item.key()) != std::string::npos) {
                        found = true;
                        CHECK(memory.at("told_by") == item.value());
                    }
                }
                CHECK(found);
            }
        } else if (step.contains("start")) {
            id = start(step.at("start"));
        } else if (step.contains("share")) {
            const nlohmann::json& share = step.at("share");
            const test::HttpJson shared = test::JsonPost(client, "/v1/runs/r/memories/share",
                                                         {{"from", share.at("from")},
                                                          {"to", share.at("to")},
                                                          {"conversation_id", share.value("conversation", firstId)}});
            REQUIRE(shared.status == 200);
            if (share.contains("shared")) {
                CHECK(shared.body.at("shared") == share.at("shared"));
            }
            if (share.contains("pending")) {
                CHECK(shared.body.at("pending") == share.at("pending"));
            }
        } else {
            FAIL("unknown step kind");
        }
    }
    harness.runs.StopAll();
    http.Stop();
}
