#include <catch2/catch_test_macros.hpp>
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

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

} // namespace

TEST_CASE("Two NPCs taking turns in one conversation share the prompt's prefix up to the tail", "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"Welcome.", "Hello there.", "Good day."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_prefix"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "And you?"), {}, {}));
    const inference::Prompt hulda = session.LastAssembly().prompt;
    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "And you, Saadia?"), {}, {}));
    const inference::Prompt saadia = session.LastAssembly().prompt;

    // Saadia's prompt starts with everything Hulda's stable prefix held.
    REQUIRE(saadia.messages.size() >= hulda.stableCount);
    for (std::size_t k = 0; k < hulda.stableCount; ++k) {
        INFO("message " << k);
        CHECK(saadia.messages[k].role == hulda.messages[k].role);
        CHECK(saadia.messages[k].content == hulda.messages[k].content);
    }
}

TEST_CASE("A fold during a turn writes the speaker's memory, in the speaker's words", "[hosted][memory]")
{
    // A transcript budget small enough that the third turn folds the oldest turns: with a 4096 context and
    // HostedSettings' budgets L3 gets about 2250 tokens, and two lines of 5000 bytes (1250 tokens each) are over it.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {std::string(5000, 'a'), std::string(5000, 'b'), "Short."};
    test::ScriptedBackend backend(4096, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = 4096;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_fold"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "One."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Two."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Three."), {}, {}));

    // Hulda, the speaker, remembers the folded turns; the fold call named her.
    CHECK(session.Conversation(id)->foldedThrough >= 1);
    const std::vector<core::Summary> hulda = session.Memories(test::HULDA);
    REQUIRE_FALSE(hulda.empty());
    CHECK(hulda[0].conversationId == id);
    bool named = false;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        named = named || call.prompt.messages.back().content.find("as Hulda remembers") != std::string::npos;
    }
    CHECK(named);
}

TEST_CASE("A hosted run's summary_cap must be below its tail_allowance", "[hosted][memory]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.budget.summaryCap = settings.budget.tailAllowance;
    CHECK_THROWS_AS(turn::HostedSession::Open(test::HostedRunFile("hosted_memory_cap"), test::FixtureHostData(), settings,
                                              42, backend),
                    core::DataError);
    CHECK_THROWS_WITH(turn::HostedSession::Open(test::HostedRunFile("hosted_memory_cap2"), test::FixtureHostData(),
                                                settings, 42, backend),
                      ContainsSubstring("summary_cap") && ContainsSubstring("tail_allowance"));
}
