#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "grammar/Schema.h"
#include "hosted/HostedTestSupport.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// A decide answer naming `id`.
std::string Decision(const std::string& id)
{
    return R"({"speaker":")" + id + R"("})";
}

turn::HostedSession OpenRun(const std::string& name, test::ScriptedBackend& backend,
                            const turn::HostData& data = test::FixtureHostData())
{
    return turn::HostedSession::Open(test::HostedRunFile(name), data, test::HostedSettings(), 42, backend);
}

// observation.json without the actor `id`.
nlohmann::json Without(const std::string& id)
{
    nlohmann::json observation = test::FixtureObservation();
    nlohmann::json& actors = observation.at("actors");
    for (auto it = actors.begin(); it != actors.end(); ++it) {
        if (it->at("id") == id) {
            actors.erase(it);
            break;
        }
    }
    return observation;
}

// The decide calls the backend saw.
std::vector<test::ScriptedBackend::Call> DecideCalls(const test::ScriptedBackend& backend)
{
    std::vector<test::ScriptedBackend::Call> calls;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        if (test::IsHostedDecide(call.prompt)) {
            calls.push_back(call);
        }
    }
    return calls;
}

} // namespace

TEST_CASE("Decide chooses among the NPC participants with a model call when more than one may speak",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA)};
    script.actions = {NONE};
    script.lines = {"Welcome."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_call", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> record =
        session.RunTurn(test::Request(id, "decide", "Good evening, both of you."), {}, {});
    REQUIRE(record);
    CHECK(record->speakerId == test::SAADIA);
    CHECK(record->listenerIds == std::vector<std::string>{test::HULDA, test::AELA});
    REQUIRE(record->timings.contains("decide"));
    CHECK_FALSE(record->timings.at("decide").contains("refused"));

    // Three calls: decide, then Saadia's resolve and speak.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 3);
    const test::ScriptedBackend::Call& decide = calls[0];
    REQUIRE(test::IsHostedDecide(decide.prompt));
    // The grammar offers the candidates' ids only; the answer fits maxTokens (6 tokens for {"speaker":"0001A6A4"} as the
    // fake counts them, plus 8).
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", {{"speaker", {{"enum", {test::HULDA, test::SAADIA}}}}}},
                                   {"required", nlohmann::json::array({"speaker"})},
                                   {"additionalProperties", false}};
    CHECK(decide.spec.grammar == grammar::FromSchema(schema));
    CHECK(decide.spec.maxTokens == 14);
    CHECK(decide.spec.thinkingMaxTokens == 0);
    // The speakers' system message; a tail with the player's line, the candidates and the decide text; no memories.
    CHECK(decide.prompt.messages[0].content == calls[1].prompt.messages[0].content);
    const std::string& tail = decide.prompt.messages.back().content;
    CHECK_THAT(tail, ContainsSubstring("# Player\nAela: Good evening, both of you.\n"));
    CHECK(tail.ends_with("# Who may speak\n- Hulda (0001A6A4)\n- Saadia (0001B07C)\n\n# Task\n" +
                         std::string(turn::HOSTED_DECIDE_INSTRUCTION)));
    CHECK_THAT(tail, !ContainsSubstring("remembers"));
}

TEST_CASE("With no input the last speaker is left out; with player text or events it may speak again",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA), Decision(test::HULDA)};
    script.actions = {NONE, NONE, NONE, NONE};
    script.lines = {"One.", "Two.", "Three.", "Four."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_last", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    // No input: Hulda spoke last, so Saadia is the one candidate: no decide call.
    const std::optional<core::HostedTurn> second = session.RunTurn(test::Request(id, "decide", ""), {}, {});
    REQUIRE(second);
    CHECK(second->speakerId == test::SAADIA);
    CHECK_FALSE(second->timings.contains("decide"));
    CHECK(DecideCalls(backend).empty());
    // Player text: both may answer, Saadia (the last speaker) included.
    const std::optional<core::HostedTurn> third = session.RunTurn(test::Request(id, "decide", "And you?"), {}, {});
    REQUIRE(third);
    CHECK(third->speakerId == test::SAADIA);
    CHECK(DecideCalls(backend).size() == 1);
    // Events and no player text: both may answer too.
    turn::HostedTurnRequest withEvents = test::Request(id, "decide", "");
    withEvents.events = {{{"text", "A door slams."}}};
    const std::optional<core::HostedTurn> fourth = session.RunTurn(withEvents, {}, {});
    REQUIRE(fourth);
    CHECK(fourth->speakerId == test::HULDA);
    CHECK(DecideCalls(backend).size() == 2);
    CHECK(script.decided == 2);
}

TEST_CASE("Decide keeps the last speaker when no one else may speak, and never offers the player or the absent",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"One.", "Two.", "Three."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_one", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    // Saadia is absent: Hulda is the one candidate, with player text and without.
    turn::HostedTurnRequest request = test::Request(id, "decide", "Hello.");
    request.observation = Without(test::SAADIA);
    const std::optional<core::HostedTurn> first = session.RunTurn(request, {}, {});
    REQUIRE(first);
    CHECK(first->speakerId == test::HULDA);
    request.playerText.reset();
    const std::optional<core::HostedTurn> second = session.RunTurn(request, {}, {});
    REQUIRE(second);
    CHECK(second->speakerId == test::HULDA); // The last speaker, as no one else may speak.
    CHECK(DecideCalls(backend).empty());
}

TEST_CASE("Decide with no NPC participant present is a DataError before any call or record", "[hosted][decide]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_none", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, "decide", "Hello?");
    request.observation = Without(test::HULDA);
    CHECK_THROWS_WITH(session.RunTurn(request, {}, {}), "speaker: \"decide\" needs an NPC participant in the observation");
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("A refused decide answer goes to the candidate who spoke least recently and keeps the reasons",
          "[hosted][decide]")
{
    test::HostedScript script;
    // The fake ignores grammars: an answer that is not JSON, then one naming the player.
    script.decisions = {"Saadia, I think.", Decision(test::AELA)};
    script.actions = {NONE, NONE, NONE};
    script.lines = {"One.", "Two.", "Three."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_refused", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    // Neither spoke yet: the tie goes to the first in join order, Hulda.
    const std::optional<core::HostedTurn> first = session.RunTurn(test::Request(id, "decide", "Hello."), {}, {});
    REQUIRE(first);
    CHECK(first->speakerId == test::HULDA);
    const nlohmann::json& refused = first->timings.at("decide").at("refused");
    REQUIRE(refused.size() == 1);
    CHECK_THAT(refused[0].get<std::string>(), ContainsSubstring("the output is not valid JSON"));

    // Hulda spoke, Saadia never did: Saadia is the least recent.
    const std::optional<core::HostedTurn> second = session.RunTurn(test::Request(id, "decide", "And now?"), {}, {});
    REQUIRE(second);
    CHECK(second->speakerId == test::SAADIA);
    CHECK_THAT(second->timings.at("decide").at("refused")[0].get<std::string>(), ContainsSubstring("/speaker"));
}

TEST_CASE("The decide prompt shows lines whoever witnessed them; the chosen speaker's prompt shows only its own",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA)};
    script.actions = {NONE, NONE};
    script.lines = {"I will keep it.", "Good evening."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_view", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    // Saadia is away while Aela tells Hulda a secret.
    turn::HostedTurnRequest secret = test::Request(id, test::HULDA, "The key is under the mat.");
    secret.observation = Without(test::SAADIA);
    REQUIRE(session.RunTurn(secret, {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, "decide", "Evening, everyone."), {}, {}));

    // The calls of the second turn: decide, then Saadia's resolve and speak.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 5);
    REQUIRE(test::IsHostedDecide(calls[2].prompt));
    auto text = [](const inference::Prompt& prompt) {
        std::string all;
        for (const inference::Message& message : prompt.messages) {
            all += message.content + "\n";
        }
        return all;
    };
    CHECK_THAT(text(calls[2].prompt), ContainsSubstring("The key is under the mat."));
    CHECK_THAT(text(calls[3].prompt), !ContainsSubstring("The key is under the mat."));
}

TEST_CASE("A host's decide text replaces the default and fills {player}", "[hosted][decide][prompts]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::HULDA)};
    script.actions = {NONE};
    script.lines = {"Yes?"};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostData data = test::FixtureHostData();
    data.prompts = {{"decide", "Who answers {player} now?"}};
    turn::HostedSession session = OpenRun("hosted_decide_pack", backend, data);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, "decide", "Anyone?"), {}, {}));
    REQUIRE_FALSE(DecideCalls(backend).empty());
    CHECK(DecideCalls(backend).front().prompt.messages.back().content.ends_with("# Task\nWho answers Aela now?"));
}

TEST_CASE("onSpeaker reports the speaker once, before the action, for a decided and a named speaker",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA)};
    script.actions = {NONE, NONE};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_on_speaker", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    std::vector<std::string> order;
    turn::HostedCallbacks callbacks;
    callbacks.onSpeaker = [&order](const std::string& speakerId) { order.push_back("speaker " + speakerId); };
    callbacks.onAction = [&order](const core::Adjudication&) { order.push_back("action"); };
    REQUIRE(session.RunTurn(test::Request(id, "decide", "Hello."), callbacks, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "And you?"), callbacks, {}));
    CHECK(order == std::vector<std::string>{"speaker " + test::SAADIA, "action", "speaker " + test::HULDA, "action"});
}

TEST_CASE("A turn stopped during its decide call commits nothing", "[hosted][decide]")
{
    std::stop_source source;
    test::ScriptedBackend backend(16384, [&source](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        REQUIRE(test::IsHostedDecide(prompt));
        source.request_stop();
        return Decision(test::SAADIA);
    });
    turn::HostedSession session = OpenRun("hosted_decide_stop", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    CHECK_FALSE(session.RunTurn(test::Request(id, "decide", "Hello."), {}, source.get_token()));
    CHECK(backend.Calls().size() == 1);
    CHECK(session.Turns(id).empty());
}
