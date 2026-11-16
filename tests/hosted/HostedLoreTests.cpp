#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// The host's lore for these tests: scoped "Wards", unscoped "Bread", and an always-on "The College" that shows from
// stage 40 of the quest MG03 on.
turn::HostData LoreData()
{
    turn::HostData data = test::FixtureHostData();
    // The schema lists the quest stage the conditions read.
    data.observationSchema["properties"]["quests"] = {
        {"type", "object"},
        {"required", nlohmann::json::array({"MG03"})},
        {"properties", {{"MG03", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}}}}},
    };
    data.lore.clear();
    data.lore.push_back({"Wards", "Wards are runes that keep intruders from a door.", false});
    data.lore.back().scopes = {"magic"};
    data.lore.push_back({"Bread", "Bread is baked at dawn for the whole inn.", false});
    // A retrieved entry with a condition and no scopes: every speaker may retrieve it from stage 40 on.
    data.lore.push_back({"Ledger", "The ledger lists the wards and the bread owed to the inn.", false});
    data.lore.back().condition = core::LoreCondition{"/quests/MG03", 40, std::nullopt, std::nullopt};
    data.lore.push_back({"The College", "The College of Winterhold teaches magic.", true});
    data.lore.back().condition = core::LoreCondition{"/quests/MG03", 40, std::nullopt, std::nullopt};
    return data;
}

// observation.json with Hulda knowing "magic", Saadia knowing nothing, and quests.MG03 at `stage`.
nlohmann::json Observation(int stage)
{
    nlohmann::json observation = test::FixtureObservation();
    for (nlohmann::json& actor : observation.at("actors")) {
        if (actor.at("id") == test::HULDA) {
            actor["knows"] = nlohmann::json::array({"magic"});
        }
    }
    observation["quests"] = {{"MG03", stage}};
    return observation;
}

// The lore subjects a turn's retrieval hit.
std::set<std::string> LoreHits(const core::HostedTurn& record)
{
    std::set<std::string> subjects;
    for (const nlohmann::json& hit : record.timings.at("retrieval")) {
        if (hit.at("source") == "lore") {
            subjects.insert(hit.at("ref").get<std::string>());
        }
    }
    return subjects;
}

// A turn of `speaker` about wards and bread on `observation`; its record.
core::HostedTurn Ask(const std::string& name, const std::string& speaker, const nlohmann::json& observation)
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hm."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile(name), LoreData(),
                                                            test::HostedSettings(),
                                                            42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, observation);
    turn::HostedTurnRequest request =
        test::Request(id, speaker, "What are wards, is there bread, and what does the ledger say?");
    request.observation = observation;
    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);
    return *record;
}

} // namespace

TEST_CASE("A scoped lore entry is retrieved by a speaker who knows its scope and by no one else", "[hosted][lore]")
{
    const core::HostedTurn hulda = Ask("hosted_lore_hulda", test::HULDA, Observation(40));
    CHECK(LoreHits(hulda).contains("Wards"));
    CHECK(LoreHits(hulda).contains("Bread"));

    const core::HostedTurn saadia = Ask("hosted_lore_saadia", test::SAADIA, Observation(40));
    CHECK_FALSE(LoreHits(saadia).contains("Wards"));
    CHECK(LoreHits(saadia).contains("Bread"));
}

TEST_CASE("A retrieved lore entry with a condition is searched only while the condition holds", "[hosted][lore]")
{
    // Saadia knows nothing and the entry has no scopes: the condition alone decides.
    CHECK_FALSE(LoreHits(Ask("hosted_lore_cond30", test::SAADIA, Observation(30))).contains("Ledger"));
    CHECK(LoreHits(Ask("hosted_lore_cond40", test::SAADIA, Observation(40))).contains("Ledger"));
}

TEST_CASE("Always-on lore with a condition is in the system message only while the condition holds", "[hosted][lore]")
{
    for (const int stage : {30, 40}) {
        INFO("stage " << stage);
        test::HostedScript script;
        script.actions = {NONE};
        script.lines = {"Hm."};
        test::ScriptedBackend backend(16384, script.Responder());
        turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_lore_always"), LoreData(),
                                                                test::HostedSettings(), 42, backend);
        const nlohmann::json observation = Observation(stage);
        const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);
        turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Hello.");
        request.observation = observation;
        REQUIRE(session.RunTurn(request, {}, {}));
        const std::string& system = session.LastAssembly().prompt.messages[0].content;
        CHECK((system.find("The College") != std::string::npos) == (stage >= 40));
    }
}

TEST_CASE("The decide prompt shows the always-on lore the observation allows", "[hosted][lore]")
{
    for (const int stage : {30, 40}) {
        INFO("stage " << stage);
        test::HostedScript script;
        script.decisions = {R"({"speaker":")" + test::HULDA + R"("})"};
        script.actions = {NONE};
        script.lines = {"Hm."};
        test::ScriptedBackend backend(16384, script.Responder());
        turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_lore_decide"), LoreData(),
                                                                test::HostedSettings(), 42, backend);
        const nlohmann::json observation = Observation(stage);
        const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, observation);
        turn::HostedTurnRequest request = test::Request(id, "decide", "Good evening.");
        request.observation = observation;
        REQUIRE(session.RunTurn(request, {}, {}));
        REQUIRE(test::IsHostedDecide(backend.Calls().front().prompt));
        const std::string& system = backend.Calls().front().prompt.messages[0].content;
        CHECK((system.find("The College") != std::string::npos) == (stage >= 40));
    }
}

TEST_CASE("A fold prompt shows the always-on lore the turn's observation allows", "[hosted][lore]")
{
    for (const int stage : {30, 40}) {
        INFO("stage " << stage);
        test::HostedScript script;
        script.actions = {NONE};
        script.lines = {"Hm."};
        test::ScriptedBackend backend(16384, script.Responder());
        turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_lore_fold"), LoreData(),
                                                                test::HostedSettings(), 42, backend);
        const nlohmann::json observation = Observation(stage);
        const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);
        turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Hello.");
        request.observation = observation;
        REQUIRE(session.RunTurn(request, {}, {}));
        session.EndConversation(id);
        REQUIRE(session.Maintain({}));
        REQUIRE(test::IsHostedFold(backend.Calls().back().prompt));
        const std::string& system = backend.Calls().back().prompt.messages[0].content;
        CHECK((system.find("The College") != std::string::npos) == (stage >= 40));
    }
}
