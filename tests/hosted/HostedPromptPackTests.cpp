#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "store/RunStore.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// A host's pack replacing the six texts it names (not decide or review); each marked "PACK" and naming its
// variables, so a test sees them filled.
std::map<std::string, std::string> CustomPack()
{
    return {{"system", "# Pack\nYou voice the people of a test scene. Act as step 1 asks, then speak as step 2 asks."},
            {"resolve", "PACK RESOLVE: what does {speaker} try while {player} waits?"},
            {"speak", "PACK SPEAK: {speaker} answers {player}."},
            {"turn_cue", "PACK CUE: {speaker} goes on."},
            {"summarize", "PACK SUMMARIZE: {actor} remembers."},
            {"condense", "PACK CONDENSE: {actor} shortens."}};
}

// open.json's host data with `pack`.
turn::HostData PackHostData(const std::map<std::string, std::string>& pack)
{
    turn::HostData data = test::FixtureHostData();
    data.prompts = pack;
    return data;
}

// A scripted model for CustomPack: a fold or a condense (its last message holds the pack's marker) answers "Summary.",
// a resolve call (it has a grammar) NONE, a speak call "Line <n>.".
test::ScriptedBackend::Responder PackResponder()
{
    return [spoken = 0](const inference::Prompt& prompt, const inference::GenerationSpec& spec) mutable -> std::string {
        const std::string& last = prompt.messages.back().content;
        if (last.starts_with("PACK SUMMARIZE") || last.find("PACK CONDENSE") != std::string::npos) {
            return "Summary.";
        }
        if (!spec.grammar.empty()) {
            return NONE;
        }
        return std::format("Line {}.", ++spoken);
    };
}

} // namespace

TEST_CASE("A host's pack replaces the system text, the instructions and the cue in every call", "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_pack_texts"),
                                                            PackHostData(CustomPack()), test::HostedSettings(), 42,
                                                            backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    // Hulda speaks first with no player text and no events (her entry shows the cue), then Aela speaks to her.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, ""), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, {}));
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 4);

    // The second resolve prompt: the pack's system text first, the first turn under the pack's cue, the pack's
    // resolve instruction last, filled.
    const inference::Prompt& resolve = calls[2].prompt;
    CHECK(resolve.messages[0].content.starts_with(CustomPack().at("system")));
    CHECK_THAT(resolve.messages[0].content, !ContainsSubstring("You voice the characters of a role-playing game"));
    CHECK(resolve.messages[1].content == "PACK CUE: Hulda goes on.");
    CHECK(resolve.messages.back().content.ends_with("# Task\nPACK RESOLVE: what does Hulda try while Aela waits?"));
    // The speak call ends with the pack's speak text, filled.
    CHECK(calls[3].prompt.messages.back().content == "PACK SPEAK: Hulda answers Aela.");

    // The fold after the end: the pack's system text, and its summarize instruction filled.
    session.EndConversation(id);
    REQUIRE(session.Maintain({}));
    CHECK(backend.Calls().back().spec.callTag == "summarize");
    const inference::Prompt& fold = backend.Calls().back().prompt;
    CHECK(fold.messages[0].content.starts_with(CustomPack().at("system")));
    CHECK(fold.messages.back().content == "PACK SUMMARIZE: Hulda remembers.");
}

TEST_CASE("A host's pack gives the condense call its instruction", "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    core::Settings settings = test::HostedSettings();
    // A summary_cap of 11: Hulda's one memory told by Saadia ("# What Hulda remembers" and "- (told by Saadia)
    // Secret.", 13 tokens) is over it, a condensed "Summary." (9 tokens) is not.
    settings.budget.summaryCap = 11;
    const std::filesystem::path file = test::HostedRunFile("hosted_pack_condense");
    const turn::HostData data = PackHostData(CustomPack());
    std::string id;
    {
        turn::HostedSession session = turn::HostedSession::Open(file, data, settings, 42, backend);
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

    // Hulda's next turn condenses her memories under the pack's condense instruction.
    turn::HostedSession session = turn::HostedSession::Open(file, data, settings, 42, backend);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Again."), {}, {}));
    bool condensed = false;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        const std::string& last = call.prompt.messages.back().content;
        if (last.find("PACK CONDENSE") != std::string::npos) {
            condensed = true;
            CHECK(call.spec.callTag == "condense");
            CHECK(last.ends_with("- (told by Saadia) Secret.\n\nPACK CONDENSE: Hulda shortens."));
        }
    }
    CHECK(condensed);
}

TEST_CASE("Under a host's pack each turn's prompt keeps the previous prompt's stable part byte for byte",
          "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_pack_prefix"),
                                                            PackHostData(CustomPack()), test::HostedSettings(), 42,
                                                            backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    std::vector<inference::Prompt> prompts;
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "How is business?"), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Goodbye."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);

    // R7's acceptance: up to the volatile tail, a prompt is the previous one's stable part plus the new entry.
    for (std::size_t t = 1; t < prompts.size(); ++t) {
        INFO("turn " << t + 1);
        const inference::Prompt& previous = prompts[t - 1];
        const inference::Prompt& next = prompts[t];
        CHECK(next.messages[0].content.starts_with(CustomPack().at("system")));
        REQUIRE(next.stableCount > previous.stableCount);
        for (std::size_t k = 0; k < previous.stableCount; ++k) {
            INFO("message " << k);
            CHECK(next.messages[k].role == previous.messages[k].role);
            CHECK(next.messages[k].content == previous.messages[k].content);
        }
    }
}

TEST_CASE("Open refuses a system text over engine_rules_cap before it touches the file", "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    core::Settings settings = test::HostedSettings();
    settings.budget.engineRulesCap = 32;
    // 500 characters: 125 tokens as the fake counts them.
    std::string longText;
    for (int i = 0; i < 100; ++i) {
        longText += "word ";
    }
    const std::filesystem::path file = test::HostedRunFile("hosted_pack_over_cap");
    CHECK_THROWS_WITH(turn::HostedSession::Open(file, PackHostData({{"system", longText}}), settings, 42, backend),
                      "prompts/system: 125 tokens, over engine_rules_cap 32");
    CHECK_FALSE(std::filesystem::exists(file));
    // A pack fault is refused before the file too.
    CHECK_THROWS_WITH(turn::HostedSession::Open(file, PackHostData({{"speak", "{who}"}}), test::HostedSettings(), 42,
                                                backend),
                      "prompts/speak: unknown variable {who}; this text takes {speaker}, {player}");
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("Reopening with a changed pack replaces the stored host data; the next prompt shows the new text",
          "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    const std::filesystem::path file = test::HostedRunFile("hosted_pack_reopen");
    std::string id;
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, PackHostData(CustomPack()), test::HostedSettings(), 42, backend);
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    }
    std::map<std::string, std::string> pack = CustomPack();
    pack["resolve"] = "PACK RESOLVE TWO: {speaker} acts.";
    const turn::HostData changed = PackHostData(pack);
    turn::HostedSession session = turn::HostedSession::Open(file, changed, test::HostedSettings(), 42, backend);
    CHECK(session.Meta().worldHash == turn::HostDataHash(changed));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Again."), {}, {}));
    CHECK(session.LastAssembly().prompt.messages.back().content.ends_with("# Task\nPACK RESOLVE TWO: Hulda acts."));
}

TEST_CASE("A resolve text over tail_allowance is a TurnError naming prompts/resolve", "[hosted][prompts]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    std::map<std::string, std::string> pack = CustomPack();
    // The fake counts 4 bytes a token: 4400 characters are over the tail's 1024 tokens on their own.
    pack["resolve"] = "PACK RESOLVE: {speaker} " + std::string(4400, 'r');
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_pack_resolve_over"),
                                                            PackHostData(pack), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    CHECK_THROWS_MATCHES(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}), turn::TurnError,
                         MessageMatches(ContainsSubstring("(prompts/resolve) use") &&
                                        ContainsSubstring("over tail_allowance")));
    CHECK(session.Turns(id).empty());
}

TEST_CASE("A speak text over narrate_allowance is a DataError naming prompts/speak", "[hosted][prompts]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    std::map<std::string, std::string> pack = CustomPack();
    // 128 tokens at 4 bytes a token: 700 characters are over narrate_allowance.
    pack["speak"] = "PACK SPEAK: {speaker} " + std::string(700, 's');
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_pack_speak_over"),
                                                            PackHostData(pack), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    CHECK_THROWS_MATCHES(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}), core::DataError,
                         MessageMatches(ContainsSubstring("prompts/speak: the speak instruction for 'Hulda' uses") &&
                                        ContainsSubstring("over narrate_allowance 128")));
    CHECK(session.Turns(id).empty());
}
