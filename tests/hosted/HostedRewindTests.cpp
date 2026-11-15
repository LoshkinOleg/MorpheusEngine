#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <limits>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// The host data's clock in these tests.
const std::string CLOCK = "/game_time/days_passed";

// observation.json at game time `days`.
nlohmann::json At(double days)
{
    nlohmann::json observation = test::FixtureObservation();
    observation["game_time"]["days_passed"] = days;
    return observation;
}

// Hulda's turn in conversation `id` after `playerText`, at game time `days`.
turn::HostedTurnRequest TurnAt(const std::string& id, const std::string& playerText, double days)
{
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, playerText);
    request.observation = At(days);
    return request;
}

// The conversations of a run played and rewound by PlayAndRewind.
struct Rewound {
    std::string open = {};  // Hulda and Aela: Hulda's turns 1 (at 1.0) and 2 (at 2.0, hidden by the rewind).
    std::string ended = {}; // Saadia and Aela, started and ended at 1.0, with no turn.
};

// Plays a run with a clock in `session` (two turns scripted), then rewinds it to 1.5: conversation `open` with Hulda's
// turns at 1.0 and 2.0 and Hulda's share of it with Saadia pending since 1.0 (the conversation is open), conversation
// `ended` started and ended at 1.0. The rewind hides turn 2 and leaves its rollback open.
Rewound PlayAndRewind(turn::HostedSession& session)
{
    Rewound run;
    run.open = session.StartConversation({test::HULDA, test::AELA}, At(1.0));
    REQUIRE(session.RunTurn(TurnAt(run.open, "Hi.", 1.0), {}, {}));
    REQUIRE(session.ShareMemories(test::HULDA, test::SAADIA, run.open).pending);
    run.ended = session.StartConversation({test::SAADIA, test::AELA}, At(1.0));
    session.EndConversation(run.ended);
    REQUIRE(session.RunTurn(TurnAt(run.open, "Again.", 2.0), {}, {}));
    REQUIRE(session.Rewind(1.5).hiddenTurns == 1);
    return run;
}

// Whether conversation `id` of `session` shows turn `number`.
bool ShowsTurn(turn::HostedSession& session, const std::string& id, int number)
{
    const std::vector<core::HostedTurn> turns = session.Turns(id);
    return std::any_of(turns.begin(), turns.end(), [number](const core::HostedTurn& turn) { return turn.number == number; });
}

} // namespace

TEST_CASE("A rewind needs a clock and a finite game time", "[hosted][rewind]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    {
        turn::HostedSession session = turn::HostedSession::Open(
            test::HostedRunFile("hosted_rewind_noclock"), test::FixtureHostData(), test::HostedSettings(), 42, backend);
        CHECK_THROWS_WITH(session.Rewind(1.0), ContainsSubstring("game_time") && ContainsSubstring("no clock"));
    }
    turn::HostData data = test::FixtureHostData();
    data.clock = CLOCK;
    turn::HostedSession session =
        turn::HostedSession::Open(test::HostedRunFile("hosted_rewind_nan"), data, test::HostedSettings(), 42, backend);
    CHECK_THROWS_WITH(session.Rewind(std::numeric_limits<double>::quiet_NaN()), ContainsSubstring("game_time"));
    CHECK_THROWS_WITH(session.Rewind(std::numeric_limits<double>::infinity()), ContainsSubstring("game_time"));
    // A run with a clock but no records yet: nothing to hide.
    const turn::RewindResult nothing = session.Rewind(1.0);
    CHECK_FALSE(nothing.rewound);
    CHECK_FALSE(nothing.rollbackTime.has_value());
}

TEST_CASE("Every new record while a rollback is open abandons it", "[hosted][rewind]")
{
    // Each writer below records something after the rewind to 1.5: the rollback is abandoned, so a rewind to 2.0 (the
    // old latest time) restores nothing and turn 2 stays hidden for good.
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})", R"({"action":"none","params":{}})",
                      R"({"action":"none","params":{}})"};
    script.lines = {"One.", "Two.", "Three."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    turn::HostData data = test::FixtureHostData();
    data.clock = CLOCK;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_rewind_abandon"), data,
                                                            test::HostedSettings(), 42, backend);
    const Rewound run = PlayAndRewind(session);

    SECTION("Events recorded")
    {
        session.RecordEvents(run.open, {nlohmann::json{{"text", "A door opens."}}});
    }
    SECTION("A conversation started")
    {
        session.StartConversation({test::HULDA, test::AELA}, At(1.2));
    }
    SECTION("Participants changed")
    {
        session.SetParticipants(run.open, {test::SAADIA}, {}, At(1.2));
    }
    SECTION("A conversation ended")
    {
        session.EndConversation(run.open);
    }
    SECTION("A share that stores a pending share")
    {
        // Hulda's memory of the open conversation is not complete yet: the share waits, stored now.
        CHECK(session.ShareMemories(test::HULDA, test::BELETHOR, run.open).pending);
    }
    SECTION("A turn committed")
    {
        REQUIRE(session.RunTurn(TurnAt(run.open, "Hello again.", 1.2), {}, {}));
    }
    const turn::RewindResult forward = session.Rewind(2.0);
    CHECK_FALSE(forward.rewound);
    CHECK(forward.restoredTurns == 0);
    CHECK(ShowsTurn(session, run.open, 1));
    CHECK_FALSE(ShowsTurn(session, run.open, 2));
}

TEST_CASE("A request that records nothing leaves the rollback open", "[hosted][rewind]")
{
    // Each request below records nothing after the rewind to 1.5: the rollback stays open, so a rewind to 2.0 (the
    // old latest time) undoes it and shows turn 2 again.
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})", R"({"action":"none","params":{}})",
                      R"({"action":"none","params":{}})"};
    script.lines = {"One.", "Two.", "Three."};
    bool failing = false; // Then every resolve call fails, as when llama-server is down.
    const test::ScriptedBackend::Responder echo = test::MemoryEchoResponder(script);
    test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (failing && !spec.grammar.empty()) {
            throw inference::InferenceError("the model is down");
        }
        return echo(prompt, spec);
    });
    turn::HostData data = test::FixtureHostData();
    data.clock = CLOCK;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_rewind_stays_open"), data,
                                                            test::HostedSettings(), 42, backend);
    const Rewound run = PlayAndRewind(session);

    SECTION("A turn cancelled through its stop token")
    {
        std::stop_source stopping;
        stopping.request_stop();
        CHECK_FALSE(session.RunTurn(TurnAt(run.open, "Hello again.", 1.2), {}, stopping.get_token()).has_value());
    }
    SECTION("A turn whose model call fails")
    {
        failing = true;
        CHECK_THROWS_AS(session.RunTurn(TurnAt(run.open, "Hello again.", 1.2), {}, {}), inference::InferenceError);
    }
    SECTION("A refused turn: an event without its text")
    {
        turn::HostedTurnRequest request = TurnAt(run.open, "Hello again.", 1.2);
        request.events = {nlohmann::json{{"note", "No text."}}};
        CHECK_THROWS_AS(session.RunTurn(request, {}, {}), core::DataError);
    }
    SECTION("Refused events: one without its text")
    {
        CHECK_THROWS_AS(session.RecordEvents(run.open, {nlohmann::json{{"note", "No text."}}}), core::DataError);
    }
    SECTION("No events")
    {
        session.RecordEvents(run.open, {});
        CHECK(session.Events(run.open).empty());
    }
    SECTION("A refused end: the conversation has ended")
    {
        CHECK_THROWS_AS(session.EndConversation(run.ended), core::DataError);
    }
    SECTION("A share that is already pending")
    {
        CHECK(session.ShareMemories(test::HULDA, test::SAADIA, run.open).pending);
    }
    const turn::RewindResult forward = session.Rewind(2.0);
    CHECK(forward.rewound);
    CHECK(forward.restoredTurns == 1);
    CHECK(ShowsTurn(session, run.open, 2));
}
