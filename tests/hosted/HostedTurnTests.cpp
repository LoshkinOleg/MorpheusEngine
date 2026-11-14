#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "grammar/Check.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"
#include "turn/Session.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

const std::string FOLLOW = R"({"action":"follow","params":{}})";
const std::string NONE = R"({"action":"none","params":{}})";

// A hosted run in a fresh file for test `name`, on `backend`.
turn::HostedSession OpenRun(const std::string& name, test::ScriptedBackend& backend)
{
    return turn::HostedSession::Open(test::HostedRunFile(name), test::FixtureHostData(), test::HostedSettings(), 42,
                                     backend);
}

} // namespace

TEST_CASE("A hosted turn resolves, adjudicates, speaks streamed and commits its record", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW};
    script.lines = {"Lead the way, then."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_turn", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    std::vector<std::string> order;
    std::string streamed;
    turn::HostedCallbacks callbacks;
    callbacks.onAction = [&](const core::Adjudication& adjudication) { order.push_back("action " + adjudication.actionId); };
    callbacks.onToken = [&](std::string_view piece) {
        if (order.back() != "token") {
            order.push_back("token");
        }
        streamed += piece;
    };
    const std::optional<core::HostedTurn> record =
        session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), callbacks, {});
    REQUIRE(record);
    CHECK(order == std::vector<std::string>{"action follow", "token"}); // The action before the line.
    CHECK(streamed == "Lead the way, then.");

    // The record of section 2, step 4.
    CHECK(record->number == 1);
    CHECK(record->conversationId == id);
    CHECK(record->speakerId == test::HULDA);
    CHECK(record->listenerIds == std::vector<std::string>{test::AELA}); // Saadia is present but takes no part.
    CHECK(record->playerText == std::optional<std::string>("Hulda, follow me."));
    CHECK(record->events.empty());
    CHECK(record->observation == test::FixtureObservation());
    CHECK(record->resolveOutput == FOLLOW);
    CHECK(record->adjudication.requested);
    CHECK(record->adjudication.actionId == "follow");
    CHECK(record->adjudication.params == nlohmann::json::object());
    CHECK(record->line == "Lead the way, then.");
    CHECK(record->timings.contains("resolve"));
    CHECK(record->timings.contains("speak"));
    CHECK(record->timings.contains("retrieval"));
    const std::vector<core::HostedTurn> turns = session.Turns(id);
    REQUIRE(turns.size() == 1);
    CHECK(turns[0].line == record->line);

    // The resolve call runs under the proposal grammar with resolve_max_tokens; the speak call extends its prompt with
    // the resolve output and the speak instruction, and has no grammar.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 2);
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Participants participants = session.Conversation(id)->participants;
    CHECK(calls[0].spec.grammar == world.ProposalGrammar(world.Observe(test::FixtureObservation()), participants,
                                                         world.ActionIds(), test::HULDA));
    CHECK(calls[0].spec.maxTokens == 64);
    const std::string& tail = calls[0].prompt.messages.back().content;
    CHECK(tail.ends_with("# Task\nStep 1: choose the action Hulda tries now. Reply with the JSON object only."));
    CHECK_THAT(tail, ContainsSubstring("# Player\nAela: Hulda, follow me.\n"));
    const inference::Prompt& speak = calls[1].prompt;
    REQUIRE(speak.messages.size() == calls[0].prompt.messages.size() + 2);
    for (std::size_t i = 0; i < calls[0].prompt.messages.size(); ++i) {
        CHECK(speak.messages[i].content == calls[0].prompt.messages[i].content);
    }
    CHECK(speak.messages[speak.messages.size() - 2].role == inference::Role::Assistant);
    CHECK(speak.messages[speak.messages.size() - 2].content == FOLLOW);
    CHECK(speak.messages.back().content ==
          "Step 2: write what Hulda says now. Spoken words only, in character. Aela is the player's character.");
    CHECK(calls[1].spec.grammar.empty());
    CHECK(calls[1].spec.maxTokens == 256);
    CHECK(session.LastAssembly().prompt.messages.size() == calls[0].prompt.messages.size());
}

TEST_CASE("A refused action is kept with its reasons, and the turn speaks with no action", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"wait","params":{},"intent":"rest"})", NONE};
    script.lines = {"As you wish.", "Still here."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_refused", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> refused = session.RunTurn(test::Request(id, test::HULDA, "Wait here."), {}, {});
    REQUIRE(refused);
    CHECK_FALSE(refused->adjudication.requested);
    CHECK(refused->adjudication.reasons ==
          std::vector<std::string>{"/intent: unexpected key 'intent'; the keys are 'action', 'params'"});
    CHECK(refused->resolveOutput == R"({"action":"wait","params":{},"intent":"rest"})");
    CHECK(refused->line == "As you wish.");
    CHECK(refused->timings.at("resolve").at("refused") == nlohmann::json(refused->adjudication.reasons));

    // The store keeps the refusal and the telemetry as they were committed.
    const std::vector<core::HostedTurn> stored = session.Turns(id);
    REQUIRE(stored.size() == 1);
    CHECK_FALSE(stored[0].adjudication.requested);
    CHECK(stored[0].adjudication.reasons == refused->adjudication.reasons);
    CHECK(stored[0].resolveOutput == refused->resolveOutput);
    CHECK(stored[0].timings == refused->timings);

    // In L3 the refused turn shows no bracket.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Are you there?"), {}, {}));
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    REQUIRE(prompt.stableCount == 3);
    CHECK(prompt.messages[1].content == "Aela: Wait here.");
    CHECK(prompt.messages[2].content == "Hulda: As you wish.");
}

TEST_CASE("A turn between NPCs needs no player text", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {NONE, FOLLOW, NONE};
    script.lines = {"Busy night.", "I will help you.", "Quiet again."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_npc", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> first = session.RunTurn(test::Request(id, test::SAADIA, ""), {}, {});
    REQUIRE(first);
    CHECK_FALSE(first->playerText);
    CHECK(first->listenerIds == std::vector<std::string>{test::HULDA});
    CHECK_THAT(backend.Calls().at(0).prompt.messages.back().content, !ContainsSubstring("# Player"));

    // Hulda answers: Saadia's turn shows in L3 with the cue where a player's line would be.
    const std::optional<core::HostedTurn> second = session.RunTurn(test::Request(id, test::HULDA, ""), {}, {});
    REQUIRE(second);
    CHECK(second->listenerIds == std::vector<std::string>{test::SAADIA});
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    REQUIRE(prompt.stableCount == 3);
    CHECK(prompt.messages[1].content == "(Saadia speaks.)");
    CHECK(prompt.messages[2].content == "Saadia [none]: Busy night.");

    // A player text of only spaces and control characters is no player text.
    const std::optional<core::HostedTurn> third = session.RunTurn(test::Request(id, test::SAADIA, " \t \n"), {}, {});
    REQUIRE(third);
    CHECK_FALSE(third->playerText);
}

TEST_CASE("The prompt is byte-identical across turns of one conversation up to the tail", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, NONE, NONE};
    script.lines = {"Lead the way, then.", "Right behind you.", "Here we are."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_prefix", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    std::vector<inference::Prompt> prompts;
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    session.RecordEvents(id, {R"({"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true,
                                 "text": "Hulda is now following Aela."})"_json});
    turn::HostedTurnRequest second = test::Request(id, test::HULDA, "To the gate.");
    second.events = {R"({"type": "ambient", "text": "The door creaks."})"_json};
    REQUIRE(session.RunTurn(second, {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Stop here."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);

    // Each prompt's stable part (the system message and L3) is the start of the next one's, message for message: a
    // new turn only adds to the end of the cached part.
    for (std::size_t k = 0; k + 1 < prompts.size(); ++k) {
        INFO("turns " << k + 1 << " and " << k + 2);
        const inference::Prompt& before = prompts[k];
        const inference::Prompt& after = prompts[k + 1];
        REQUIRE(after.stableCount == before.stableCount + 2);
        for (std::size_t i = 0; i < before.stableCount; ++i) {
            CHECK(after.messages[i].role == before.messages[i].role);
            CHECK(after.messages[i].content == before.messages[i].content);
        }
    }

    // The second turn's tail holds both new events; the third's L3 shows them before the player's line.
    CHECK_THAT(prompts[1].messages.back().content,
               ContainsSubstring("# New events\n(Hulda is now following Aela.)\n(The door creaks.)\n"));
    const inference::Prompt& last = prompts[2];
    REQUIRE(last.stableCount == 5);
    CHECK(last.messages[1].content == "Aela: Hulda, follow me.");
    CHECK(last.messages[2].content == "Hulda [follow]: Lead the way, then.");
    CHECK(last.messages[3].content == "(Hulda is now following Aela.)\n(The door creaks.)\nAela: To the gate.");
    CHECK(last.messages[4].content == "Hulda [none]: Right behind you.");
    CHECK_THAT(last.messages.back().content, !ContainsSubstring("# New events"));
}

TEST_CASE("A join or a leave changes the prompt only from the participants layer on", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, NONE, NONE};
    script.lines = {"Lead the way, then.", "Welcome, Saadia.", "Just us now."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_join", backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);

    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), {}, {}));
    const inference::Prompt before = session.LastAssembly().prompt;
    session.SetParticipants(id, {test::SAADIA}, {}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Saadia, join us."), {}, {}));
    const inference::Prompt joined = session.LastAssembly().prompt;

    // Saadia joins at the end of the participants layer, which ends the system message here (no summary yet): the
    // first turn's system message is the start of the second's.
    const std::string& a = before.messages[0].content;
    const std::string& b = joined.messages[0].content;
    CHECK(b.starts_with(a));
    CHECK(b.substr(a.size()) == "- Saadia (0001B07C): Saadia serves at the inn.\n");
    CHECK(joined.messages[1].content == "Aela: Hulda, follow me.");

    // A leave changes the system message from the leaver's line on: L0, L1 and the participants before it stay.
    session.SetParticipants(id, {}, {test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, ""), {}, {}));
    const std::string c = session.LastAssembly().prompt.messages[0].content;
    const std::size_t aelaLine = b.find("- Aela (00000007)\n");
    REQUIRE(aelaLine != std::string::npos);
    CHECK(c.substr(0, aelaLine) == b.substr(0, aelaLine));
    CHECK(c.substr(aelaLine) == "- Saadia (0001B07C): Saadia serves at the inn.\n");
}

TEST_CASE("A cancel during the resolve or the speak call commits nothing", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, FOLLOW, FOLLOW};
    script.lines = {"Lead the way.", "Lead the way."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_cancel", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Follow me.");
    request.events = {R"({"text": "Rain starts."})"_json};

    // Cancelled before the resolve call writes anything.
    std::stop_source early;
    early.request_stop();
    CHECK_FALSE(session.RunTurn(request, {}, early.get_token()));

    // Cancelled while the line streams: onAction asks for the stop, so the speak call ends at its first piece.
    std::stop_source late;
    bool acted = false;
    turn::HostedCallbacks callbacks;
    callbacks.onAction = [&](const core::Adjudication&) {
        acted = true;
        late.request_stop();
    };
    CHECK_FALSE(session.RunTurn(request, callbacks, late.get_token()));
    CHECK(acted);
    CHECK(session.Turns(id).empty());
    CHECK(session.Events(id).empty()); // The request's events were not kept either.

    // The same request then plays as turn 1, with its events.
    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);
    CHECK(record->number == 1);
    CHECK(record->events == request.events);
}

TEST_CASE("Each malformed request is a DataError naming its field, before any model call", "[hosted][turn]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_bad_requests", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    const std::string ended = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.EndConversation(ended);
    // RunTurn of `request` is a DataError whose message holds `field`.
    auto fails = [&](const turn::HostedTurnRequest& request, const std::string& field) {
        INFO(field);
        CHECK_THROWS_MATCHES(session.RunTurn(request, {}, {}), core::DataError, MessageMatches(ContainsSubstring(field)));
    };

    fails(test::Request("c9", test::HULDA, "Hi."), "conversation_id: there is no conversation 'c9'");
    fails(test::Request(ended, test::HULDA, "Hi."), "conversation_id: conversation 'c2' has ended");
    turn::HostedTurnRequest badObservation = test::Request(id, test::HULDA, "Hi.");
    badObservation.observation.at("actors").at(0)["id"] = "";
    fails(badObservation, "observation at /actors/0/id");
    fails(test::Request(id, test::SAADIA, "Hi."), "speaker: '0001B07C' is not a participant of conversation 'c1'");
    fails(test::Request(id, test::AELA, "Hi."), "speaker: '00000007' is the player");
    turn::HostedTurnRequest absent = test::Request(id, test::HULDA, "Hi.");
    nlohmann::json& actors = absent.observation.at("actors");
    actors.erase(actors.begin()); // Hulda is gone; Saadia and Aela stay.
    fails(absent, "speaker: '0001A6A4' is not an actor of the observation");
    turn::HostedTurnRequest unknownAction = test::Request(id, test::HULDA, "Hi.");
    unknownAction.allowedActions = std::vector<std::string>{"follow", "fly"};
    fails(unknownAction, "allowed_actions/1: 'fly' is not an action of the catalog");
    turn::HostedTurnRequest noTarget = test::Request(id, test::HULDA, "Hi.");
    nlohmann::json& present = noTarget.observation.at("actors");
    present.erase(present.begin() + 1); // Saadia is gone: Hulda has no one but the player to attack.
    REQUIRE(present.at(0).at("id") == test::HULDA);
    noTarget.allowedActions = std::vector<std::string>{"attack"};
    fails(noTarget, "allowed_actions: no allowed action can be taken this turn");
    turn::HostedTurnRequest badEvent = test::Request(id, test::HULDA, "Hi.");
    badEvent.events = {R"({"type": "ambient"})"_json};
    fails(badEvent, "events/0/text: an event needs 'text', a string");

    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("A speak instruction over narrate_allowance is a DataError naming the speaker, before any model call",
          "[hosted][turn]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.budget.narrateAllowance = 4;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_speak_allowance"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    CHECK_THROWS_MATCHES(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}), core::DataError,
                         MessageMatches(ContainsSubstring("speaker: the speak instruction for 'Hulda' uses") &&
                                        ContainsSubstring("over narrate_allowance 4")));
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("CleanInput drops control characters but newline and tab, and trims", "[hosted][turn]")
{
    const std::string input = std::string(" \t a") + static_cast<char>(0x01) + "b\tc\nd" + static_cast<char>(0x7F) +
                              static_cast<char>(0x1B) + "e" + static_cast<char>(0xC3) + static_cast<char>(0xA9) +
                              " \n";
    CHECK(turn::CleanInput(input) ==
          std::string("ab\tc\nde") + static_cast<char>(0xC3) + static_cast<char>(0xA9)); // UTF-8 bytes stay.
    CHECK(turn::CleanInput(" \t \n").empty());
    CHECK(turn::CleanInput(std::string(1, static_cast<char>(0x07))).empty());
}

TEST_CASE("A player text with no room in the tail allowance is a TurnError", "[hosted][turn]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_long_text", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    const turn::HostedTurnRequest request = test::Request(id, test::HULDA, std::string(5000, 'x'));
    CHECK_THROWS_AS(session.RunTurn(request, {}, {}), turn::TurnError);
    CHECK_THROWS_WITH(session.RunTurn(request, {}, {}),
                      ContainsSubstring("the observation, the events and the player text use"));
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("allowedActions narrows the grammar the resolve call runs under and the schema it is checked against",
          "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"attack","params":{"target":"0001B07C"}})", NONE};
    script.lines = {"Leave, now.", "Fine."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_allowed", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Throw her out.");
    request.allowedActions = std::vector<std::string>{"follow", "attack"};

    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);
    CHECK(record->adjudication.requested);
    CHECK(record->adjudication.params == R"({"target": "0001B07C"})"_json);
    const std::string& gbnf = backend.Calls().at(0).spec.grammar;
    CHECK(grammar::Check(gbnf, FOLLOW).empty());
    CHECK(grammar::Check(gbnf, R"({"action":"attack","params":{"target":"0001B07C"}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, NONE).empty());

    // An action the turn does not allow is refused even when the model writes it.
    const std::optional<core::HostedTurn> next = session.RunTurn(request, {}, {});
    REQUIRE(next);
    CHECK_FALSE(next->adjudication.requested);
    REQUIRE(next->adjudication.reasons.size() == 1);
    CHECK(next->adjudication.reasons[0].starts_with("/action: unknown action 'none'"));
}
