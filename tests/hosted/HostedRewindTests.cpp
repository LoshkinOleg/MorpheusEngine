#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

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
    data.clock = "/game_time/days_passed";
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
    // A run with a clock: one turn at 1.0, one at 2.0; rewind to 1.5; then recording events abandons the rollback,
    // and a rewind to 3.0 restores nothing.
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})", R"({"action":"none","params":{}})"};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    turn::HostData data = test::FixtureHostData();
    data.clock = "/game_time/days_passed";
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_rewind_abandon"), data,
                                                            test::HostedSettings(), 42, backend);
    nlohmann::json at1 = test::FixtureObservation();
    at1["game_time"]["days_passed"] = 1.0;
    nlohmann::json at2 = at1;
    at2["game_time"]["days_passed"] = 2.0;
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, at1);
    turn::HostedTurnRequest first = test::Request(id, test::HULDA, "Hi.");
    first.observation = at1;
    REQUIRE(session.RunTurn(first, {}, {}));
    turn::HostedTurnRequest second = test::Request(id, test::HULDA, "Again.");
    second.observation = at2;
    REQUIRE(session.RunTurn(second, {}, {}));

    CHECK(session.Rewind(1.5).hiddenTurns == 1);
    session.RecordEvents(id, {nlohmann::json{{"text", "A door opens."}}});
    const turn::RewindResult forward = session.Rewind(3.0);
    CHECK_FALSE(forward.rewound);
    CHECK(forward.restoredTurns == 0);
    CHECK(session.Turns(id).size() == 1);
}
