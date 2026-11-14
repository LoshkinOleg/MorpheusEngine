#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

// The request a fixture's "turn" describes, in conversation `conversationId`. Without an "observation" it carries
// observation.json.
turn::HostedTurnRequest RequestFrom(const nlohmann::json& turnJson, const std::string& conversationId)
{
    turn::HostedTurnRequest request;
    request.conversationId = conversationId;
    request.speakerId = turnJson.at("speaker").get<std::string>();
    if (turnJson.contains("player_text")) {
        request.playerText = turnJson.at("player_text").get<std::string>();
    }
    if (turnJson.contains("events")) {
        request.events = turnJson.at("events").get<std::vector<nlohmann::json>>();
    }
    request.observation = turnJson.contains("observation") ? turnJson.at("observation") : test::FixtureObservation();
    if (turnJson.contains("allowed_actions")) {
        request.allowedActions = turnJson.at("allowed_actions").get<std::vector<std::string>>();
    }
    return request;
}

// Plays the conversation fixture `name` with the fake backend replying as its steps say, and checks each turn
// against its "expect": the record, the L3 its prompt showed and the new events its tail listed.
void PlayFixture(const std::string& name)
{
    const nlohmann::json fixture = test::ReadFixture(name);
    const nlohmann::json& steps = fixture.at("steps");
    test::HostedScript script;
    for (const nlohmann::json& step : steps) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fixture_" + name),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation(fixture.at("participants").get<std::vector<std::string>>(),
                                                     test::FixtureObservation());

    int played = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const nlohmann::json& step = steps[i];
        INFO(name << " step " << i);
        if (step.contains("events")) {
            session.RecordEvents(id, step.at("events").get<std::vector<nlohmann::json>>());
            continue;
        }
        const std::optional<core::HostedTurn> record = session.RunTurn(RequestFrom(step.at("turn"), id), {}, {});
        REQUIRE(record);
        ++played;

        // The record.
        const nlohmann::json& expect = step.at("expect");
        CHECK(record->number == played);
        CHECK(record->adjudication.requested == expect.at("requested").get<bool>());
        if (expect.contains("action")) {
            CHECK(record->adjudication.actionId == expect.at("action").at("id").get<std::string>());
            CHECK(record->adjudication.params == expect.at("action").at("params"));
        }
        if (expect.contains("reasons")) {
            CHECK(record->adjudication.reasons == expect.at("reasons").get<std::vector<std::string>>());
        }
        CHECK(record->line == expect.at("line").get<std::string>());
        CHECK(record->listenerIds == expect.at("listeners").get<std::vector<std::string>>());

        // The turn's prompt: its L3 messages, and the new events in its tail.
        const inference::Prompt& prompt = session.LastAssembly().prompt;
        if (expect.contains("l3")) {
            // Message 0 is the stable system message (L0); L3 starts at message 1 and runs to stableCount.
            std::vector<std::string> l3;
            for (std::size_t k = 1; k < prompt.stableCount; ++k) {
                l3.push_back(prompt.messages[k].content);
            }
            CHECK(l3 == expect.at("l3").get<std::vector<std::string>>());
        }
        if (expect.contains("new_events")) {
            const std::vector<std::string> events = expect.at("new_events").get<std::vector<std::string>>();
            const std::string& tail = prompt.messages.back().content;
            if (events.empty()) {
                CHECK_THAT(tail, !ContainsSubstring("# New events"));
            } else {
                std::string block = "# New events\n";
                for (const std::string& event : events) {
                    block += "(" + event + ")\n";
                }
                CHECK_THAT(tail, ContainsSubstring(block));
            }
        }
    }
    CHECK(session.Turns(id).size() == static_cast<std::size_t>(played));
}

} // namespace

TEST_CASE("The scripted conversation fixture plays with the records and the L3 it expects", "[hosted][fixtures]")
{
    PlayFixture("conversation.json");
}

TEST_CASE("The radiant fixture: two NPCs talk for six turns with no player text", "[hosted][fixtures]")
{
    PlayFixture("radiant.json");
}

TEST_CASE("Each invalid request fixture is a DataError naming its field, before any model call", "[hosted][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("invalid_requests.json");
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fixture_invalid"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::vector<std::string> participants = fixture.at("participants").get<std::vector<std::string>>();
    const std::string open = session.StartConversation(participants, test::FixtureObservation());
    const std::string ended = session.StartConversation(participants, test::FixtureObservation());
    session.EndConversation(ended);

    for (const nlohmann::json& entry : fixture.at("requests")) {
        INFO(entry.at("name").get<std::string>());
        const std::string named = entry.at("conversation").get<std::string>();
        const std::string conversationId = named == "open" ? open : named == "ended" ? ended : named;
        CHECK_THROWS_MATCHES(session.RunTurn(RequestFrom(entry.at("turn"), conversationId), {}, {}), core::DataError,
                             MessageMatches(ContainsSubstring(entry.at("error").get<std::string>())));
    }
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(open).empty());
}
