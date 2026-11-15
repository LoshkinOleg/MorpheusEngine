#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "store/RunStore.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// The L3 messages of `prompt`: those after the system message, within the stable part.
std::vector<std::string> L3(const inference::Prompt& prompt)
{
    std::vector<std::string> messages;
    for (std::size_t i = 1; i < prompt.stableCount; ++i) {
        messages.push_back(prompt.messages[i].content);
    }
    return messages;
}

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

TEST_CASE("A speaker's L3 holds only the conversation's turns it witnessed", "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE, NONE};
    script.lines = {"Welcome to the Bannered Mare.", "Mead is ten septims.", "I only just got here.", "Saadia, sit."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_witness"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Mead?"), {}, {}));

    // Saadia joins after the first two turns: she witnessed neither, so her L3 holds neither.
    session.SetParticipants(id, {test::SAADIA}, {}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "And you, Saadia?"), {}, {}));
    const std::vector<std::string> saadia = L3(session.LastAssembly().prompt);
    for (const std::string& message : saadia) {
        CHECK(message.find("Bannered Mare.") == std::string::npos);
        CHECK(message.find("ten septims") == std::string::npos);
    }
    CHECK(saadia.empty());

    // Hulda witnessed all three: her next L3 holds the first two turns and Saadia's.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hulda?"), {}, {}));
    CHECK(L3(session.LastAssembly().prompt) ==
          std::vector<std::string>{"Aela: Hello.", "Hulda [none]: Welcome to the Bannered Mare.", "Aela: Mead?",
                                   "Hulda [none]: Mead is ten septims.", "Aela: And you, Saadia?",
                                   "Saadia [none]: I only just got here."});
}

TEST_CASE("A hosted NPC's name that holds {actor} fills the hosted texts once", "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Welcome."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_actor_name"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    // Hulda goes by a name that holds the variable the engine fills.
    nlohmann::json observation = test::FixtureObservation();
    for (nlohmann::json& actor : observation.at("actors")) {
        if (actor.at("id") == test::HULDA) {
            actor["name"] = "{actor} the Bold";
        }
    }
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Hello.");
    request.observation = observation;
    REQUIRE(session.RunTurn(request, {}, {}));
    session.EndConversation(id);
    REQUIRE(session.Maintain({}));

    // The fold call names her where the text has {actor}, and nowhere else.
    REQUIRE(test::IsHostedFold(backend.Calls().back().prompt));
    CHECK(backend.Calls().back().prompt.messages.back().content ==
          "Summarize the turns above as {actor} the Bold remembers them, for {actor} the Bold's own memory, not for the "
          "player. Keep names, places, items, promises, secrets and who said what to whom. Leave out JSON and engine "
          "details. Plain prose, at most 200 words.");
}

TEST_CASE("A hosted condense shows a memory told by another NPC under the teller's name", "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"Welcome.", "Hello again."};
    test::ScriptedBackend backend(16384, script.Responder());
    core::Settings settings = test::HostedSettings();
    // A summary_cap of 11: Hulda's one memory told by Saadia ("# What Hulda remembers" and "- (told by Saadia)
    // Secret.", 13 tokens) is over it, a condensed "Summary." (9 tokens) is not.
    settings.budget.summaryCap = 11;
    const std::filesystem::path file = test::HostedRunFile("hosted_memory_told_by");
    std::string id;
    {
        turn::HostedSession session = turn::HostedSession::Open(file, test::FixtureHostData(), settings, 42, backend);
        id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, {}));
    }
    // Saadia told Hulda a secret: a shared copy, written to the closed run's store as the share route would.
    {
        store::RunStore store = store::RunStore::Open(file);
        core::Summary told;
        told.fromTurn = 1;
        told.toTurn = 1;
        told.text = "Secret.";
        told.actorId = test::HULDA;
        told.conversationId = id;
        told.toldBy = test::SAADIA;
        store.AddActorSummary(told);
    }

    // Hulda's next turn condenses her memories; the condense call names the teller.
    turn::HostedSession session = turn::HostedSession::Open(file, test::FixtureHostData(), settings, 42, backend);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Again."), {}, {}));
    bool condensed = false;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        if (test::IsHostedCondense(call.prompt)) {
            condensed = true;
            CHECK_THAT(call.prompt.messages.back().content,
                       ContainsSubstring("# What Hulda remembers\n- (told by Saadia) Secret.\n\nCondense what Hulda"));
        }
    }
    CHECK(condensed);
}
