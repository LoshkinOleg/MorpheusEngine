#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// For a context of 2600 with lines of 2400 bytes. FakeBackend counts 1 token per 4 bytes: the hosted system message
// is about 350 tokens, so L3 gets about 1400 while each turn adds about 615. Two turns fit the conversation, three do
// not. The tail (the observation is about 190 tokens) stays well within its 600.
core::Settings FoldingSettings()
{
    core::Settings settings = test::HostedSettings();
    settings.budget = {64, 128, 64, 600, 1024, 1024, 512, 100};
    return settings;
}

} // namespace

TEST_CASE("A conversation over its transcript budget folds its oldest turns during a turn", "[hosted][folding]")
{
    test::HostedScript script;
    for (int i = 0; i < 6; ++i) {
        script.actions.push_back(NONE);
        script.lines.push_back(std::string(2400, 'w'));
    }
    test::ScriptedBackend backend(2600, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_budget"),
                                                            test::FixtureHostData(), FoldingSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    for (int i = 0; i < 6; ++i) {
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Go on."), {}, {}));
        CHECK(session.LastAssembly().Fits());
    }

    // The oldest turns went into run-wide summaries, and the conversation's folded mark moved with them.
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE_FALSE(summaries.empty());
    CHECK(summaries.front().fromTurn == 1);
    CHECK(session.Conversation(id)->foldedThrough >= 1);
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    CHECK(prompt.stableCount < 1 + 2 * 5);
    CHECK_THAT(prompt.messages.front().content, ContainsSubstring("# Story so far\nTurns 1-"));
}

TEST_CASE("EndConversation and Maintain fold the rest, and a later conversation retrieves it", "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"The silver key is under the bar.", "Nobody else knows about the silver key.", "Under the bar."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_end"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string first = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "Where did you hide the silver key?"), {}, {}));
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "Does anyone else know?"), {}, {}));

    // While the conversation is open, Maintain leaves it alone.
    CHECK(session.Maintain({}));
    CHECK(session.Summaries().empty());

    // Ended, its remaining turns fold in one call.
    session.EndConversation(first);
    CHECK(session.Maintain({}));
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text == "Summary.");
    CHECK(session.Conversation(first)->foldedThrough == 2);
    const std::size_t calls = backend.Calls().size();
    CHECK(session.Maintain({})); // Nothing is left to fold.
    CHECK(backend.Calls().size() == calls);

    // A later conversation sees the summary in L2, starts with an empty L3, and retrieves the folded turns.
    const std::string second = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(second, test::HULDA, "Remind me where the silver key is."), {}, {}));
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    CHECK_THAT(prompt.messages.front().content, ContainsSubstring("# Story so far\nTurns 1-2: Summary.\n"));
    CHECK(prompt.stableCount == 1);
    CHECK_THAT(prompt.messages.back().content,
               ContainsSubstring("\n- Turn 1: Aela: Where did you hide the silver key? Hulda [none]: The silver key "
                                 "is under the bar.\n"));
}

TEST_CASE("A cancelled Maintain folds nothing and can simply run again", "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_cancel"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "First."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Second."), {}, {}));
    session.EndConversation(id);

    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(session.Maintain(stop.get_token()));
    CHECK(session.Summaries().empty());
    CHECK(session.Conversation(id)->foldedThrough == 0);

    CHECK(session.Maintain({}));
    REQUIRE(session.Summaries().size() == 1);
    CHECK(session.Conversation(id)->foldedThrough == 2);
}
