#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <cstdint>
#include <span>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// The turn numbers of `turns`, in order.
std::vector<int> Numbers(const std::vector<core::HostedTurn>& turns)
{
    std::vector<int> numbers;
    for (const core::HostedTurn& turn : turns) {
        numbers.push_back(turn.number);
    }
    return numbers;
}

// Delegates every call to a ScriptedBackend; once armed, it requests `stop` after each Embed. In Maintain the only
// embeds index a landed fold's turns as history, so the stop lands right after the first fold, before the next.
class StopAfterEmbed final : public inference::LlmBackend {
private:
    test::ScriptedBackend* _pInner = nullptr; // Not owned; outlives this.
    std::stop_source* _pStop = nullptr;       // Not owned; outlives this.
    bool _armed = false;

public:
    StopAfterEmbed(test::ScriptedBackend& inner, std::stop_source& stop)
        : _pInner(&inner)
        , _pStop(&stop)
    {
    }

    void Arm()
    {
        _armed = true;
    }

    int CountTokens(std::string_view text) override
    {
        return _pInner->CountTokens(text);
    }

    int CountPromptTokens(const inference::Prompt& prompt) override
    {
        return _pInner->CountPromptTokens(prompt);
    }

    int ContextSize() const override
    {
        return _pInner->ContextSize();
    }

    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override
    {
        return _pInner->Complete(prompt, spec, onToken, stop);
    }

    std::vector<std::uint8_t> SaveState() override
    {
        return _pInner->SaveState();
    }

    void RestoreState(std::span<const std::uint8_t> state) override
    {
        _pInner->RestoreState(state);
    }

    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override
    {
        std::vector<std::vector<float>> vectors = _pInner->Embed(texts, purpose);
        if (_armed) {
            _pStop->request_stop();
        }
        return vectors;
    }

    std::string EmbeddingModelId() const override
    {
        return _pInner->EmbeddingModelId();
    }
};

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

    // The oldest turns went into the speaker's summaries, and the conversation's folded mark moved with them.
    const std::vector<core::Summary> summaries = session.Memories(test::HULDA);
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
    CHECK(session.Memories(test::HULDA).empty());

    // Ended, its remaining turns fold in one call.
    session.EndConversation(first);
    CHECK(session.Maintain({}));
    const std::vector<core::Summary> summaries = session.Memories(test::HULDA);
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
    CHECK(session.Memories(test::HULDA).empty());
    CHECK(session.Conversation(id)->foldedThrough == 0);

    CHECK(session.Maintain({}));
    REQUIRE(session.Memories(test::HULDA).size() == 1);
    CHECK(session.Conversation(id)->foldedThrough == 2);
}

TEST_CASE("Two open conversations interleave their turns; ending one folds only its turns, which the other retrieves",
          "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE, NONE, NONE};
    script.lines = {"The silver key is under the bar.", "Only about the bard.",
                    "Nobody else knows about the silver key.", "He sings of dragons.", "Under the bar, they say."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_interleaved"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string c1 = session.StartConversation({test::HULDA, test::AELA}, observation);
    const std::string c2 = session.StartConversation({test::SAADIA, test::AELA}, observation);
    // The L3 messages of the last resolve prompt: those after the system message, within the stable part.
    auto transcript = [&session] {
        const inference::Prompt& prompt = session.LastAssembly().prompt;
        std::vector<std::string> messages;
        for (std::size_t i = 1; i < prompt.stableCount; ++i) {
            messages.push_back(prompt.messages[i].content);
        }
        return messages;
    };
    auto tail = [&session] { return session.LastAssembly().prompt.messages.back().content; };

    // Turns alternate, with events recorded in each conversation between them. Turn numbers run across the run.
    session.RecordEvents(c1, {nlohmann::json{{"text", "Rain starts."}}});
    REQUIRE(session.RunTurn(test::Request(c1, test::HULDA, "Where did you hide the silver key?"), {}, {})->number == 1);
    CHECK(transcript().empty());
    CHECK_THAT(tail(), ContainsSubstring("# New events\n(Rain starts.)\n"));

    session.RecordEvents(c2, {nlohmann::json{{"text", "A bard tunes a lute."}}});
    REQUIRE(session.RunTurn(test::Request(c2, test::SAADIA, "Any rumours tonight?"), {}, {})->number == 2);
    CHECK(transcript().empty()); // c1's turn is not in c2's L3.
    CHECK_THAT(tail(), ContainsSubstring("# New events\n(A bard tunes a lute.)\n"));
    CHECK_THAT(tail(), !ContainsSubstring("Rain starts."));

    session.RecordEvents(c1, {nlohmann::json{{"text", "The door creaks."}}});
    REQUIRE(session.RunTurn(test::Request(c1, test::HULDA, "Does anyone else know?"), {}, {})->number == 3);
    CHECK(transcript() == std::vector<std::string>{"(Rain starts.)\nAela: Where did you hide the silver key?",
                                                   "Hulda [none]: The silver key is under the bar."});
    CHECK_THAT(tail(), ContainsSubstring("# New events\n(The door creaks.)\n"));
    CHECK_THAT(tail(), !ContainsSubstring("lute"));

    session.RecordEvents(c2, {nlohmann::json{{"text", "The fire crackles."}}});
    REQUIRE(session.RunTurn(test::Request(c2, test::SAADIA, "Tell me about the bard."), {}, {})->number == 4);
    CHECK(transcript() == std::vector<std::string>{"(A bard tunes a lute.)\nAela: Any rumours tonight?",
                                                   "Saadia [none]: Only about the bard."});
    CHECK_THAT(tail(), ContainsSubstring("# New events\n(The fire crackles.)\n"));
    CHECK_THAT(tail(), !ContainsSubstring("door creaks"));

    // Each conversation holds its own turns, and each event follows its own conversation's latest turn.
    CHECK(Numbers(session.Turns(c1)) == std::vector<int>{1, 3});
    CHECK(Numbers(session.Turns(c2)) == std::vector<int>{2, 4});
    const std::vector<core::ConversationEvent> events1 = session.Events(c1);
    REQUIRE(events1.size() == 2);
    CHECK(events1[0].afterTurn == 0);
    CHECK(events1[1].afterTurn == 1);
    const std::vector<core::ConversationEvent> events2 = session.Events(c2);
    REQUIRE(events2.size() == 2);
    CHECK(events2[0].afterTurn == 0);
    CHECK(events2[1].afterTurn == 2);

    // c1 ends; Maintain folds its turns (1 and 3) in one call, which holds none of c2's, and leaves c2 alone.
    session.EndConversation(c1);
    const std::size_t calls = backend.Calls().size();
    CHECK(session.Maintain({}));
    REQUIRE(backend.Calls().size() == calls + 1);
    const inference::Prompt& fold = backend.Calls().back().prompt;
    CHECK(fold.messages.back().content == std::string(turn::SUMMARIZE_INSTRUCTION));
    std::string folded;
    for (std::size_t i = 1; i < fold.messages.size(); ++i) {
        folded += fold.messages[i].content + "\n";
    }
    CHECK_THAT(folded, ContainsSubstring("Hulda [none]: The silver key is under the bar."));
    CHECK_THAT(folded, ContainsSubstring("Hulda [none]: Nobody else knows about the silver key."));
    CHECK_THAT(folded, !ContainsSubstring("Saadia") && !ContainsSubstring("bard"));
    const std::vector<core::Summary> summaries = session.Memories(test::HULDA);
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 3);
    CHECK(session.Conversation(c1)->foldedThrough == 3);
    CHECK(session.Conversation(c2)->foldedThrough == 0);

    // The summary is Hulda's, who spoke c1's last turn; Saadia has none.
    CHECK(session.Memories(test::SAADIA).empty());

    // c2's next turn (Saadia's) has no summary of c1 in L2 and its own turns still in L3, and retrieves c1's folded
    // turns.
    REQUIRE(session.RunTurn(test::Request(c2, test::SAADIA, "Where is the silver key?"), {}, {})->number == 5);
    CHECK_THAT(session.LastAssembly().prompt.messages.front().content, !ContainsSubstring("# Story so far"));
    CHECK(transcript() == std::vector<std::string>{"(A bard tunes a lute.)\nAela: Any rumours tonight?",
                                                   "Saadia [none]: Only about the bard.",
                                                   "(The fire crackles.)\nAela: Tell me about the bard.",
                                                   "Saadia [none]: He sings of dragons."});
    CHECK_THAT(tail(), ContainsSubstring("\n- Turn 1: (Rain starts.) Aela: Where did you hide the silver key? Hulda "
                                         "[none]: The silver key is under the bar.\n"));
    CHECK(session.Conversation(c2)->foldedThrough == 0);
}

TEST_CASE("A stop between two ended conversations in one Maintain keeps the first fold, and the next Maintain folds "
          "the second",
          "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend inner(16384, script.Responder());
    std::stop_source stop;
    StopAfterEmbed backend(inner, stop);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_stop_between"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string c1 = session.StartConversation({test::HULDA, test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(c1, test::HULDA, "First."), {}, {}));
    const std::string c2 = session.StartConversation({test::SAADIA, test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(c2, test::SAADIA, "Second."), {}, {}));
    session.EndConversation(c1);
    session.EndConversation(c2);

    // The stop is requested once c1's fold has landed and its turn is indexed: c2's fold call is cancelled.
    backend.Arm();
    CHECK_FALSE(session.Maintain(stop.get_token()));
    REQUIRE(session.Memories(test::HULDA).size() == 1);
    CHECK(session.Memories(test::HULDA)[0].fromTurn == 1);
    CHECK(session.Memories(test::HULDA)[0].toTurn == 1);
    CHECK(session.Conversation(c1)->foldedThrough == 1);
    CHECK(session.Conversation(c2)->foldedThrough == 0);

    // The next Maintain folds only what is left: c2.
    const std::size_t calls = inner.Calls().size();
    CHECK(session.Maintain({}));
    CHECK(inner.Calls().size() == calls + 1);
    // c1's fold is Hulda's, who spoke its last turn; c2's is Saadia's.
    CHECK(session.Memories(test::HULDA).size() == 1);
    const std::vector<core::Summary> summaries = session.Memories(test::SAADIA);
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 2);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].conversationId == c2);
    CHECK(session.Conversation(c1)->foldedThrough == 1);
    CHECK(session.Conversation(c2)->foldedThrough == 2);
}

TEST_CASE("A speaker's memories over summary_cap condense into one, superseding the old ones and leaving others' alone",
          "[hosted][folding]")
{
    // A summary_cap of 12: one memory ("# Story so far" and one line, 9 tokens) fits, two (14 tokens) do not.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"One.", "Two.", "Three."};
    test::ScriptedBackend backend(16384, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.budget.summaryCap = 12;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_condense_speaker"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const nlohmann::json observation = test::FixtureObservation();

    // Saadia keeps one memory of her own; Hulda folds two conversations, one memory each.
    const std::string c1 = session.StartConversation({test::SAADIA, test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(c1, test::SAADIA, "First."), {}, {}));
    session.EndConversation(c1);
    REQUIRE(session.Maintain({}));
    const std::string c2 = session.StartConversation({test::HULDA, test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(c2, test::HULDA, "Second."), {}, {}));
    session.EndConversation(c2);
    REQUIRE(session.Maintain({}));
    REQUIRE(session.Memories(test::HULDA).size() == 1);
    const std::size_t calls = backend.Calls().size();

    // The third conversation's fold gives Hulda two memories, over her cap: the upkeep condenses them through her
    // scope, in one condense call.
    const std::string c3 = session.StartConversation({test::HULDA, test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(c3, test::HULDA, "Third."), {}, {}));
    session.EndConversation(c3);
    CHECK(session.Maintain({}));
    REQUIRE(backend.Calls().size() == calls + 4); // The turn's two calls, then the fold and the condense.
    CHECK(backend.Calls().back().prompt.messages.back().content == std::string(turn::CONDENSE_INSTRUCTION));

    // Hulda shows one condensed memory over both conversations' turns, with no conversation; the two it condensed stay,
    // superseded.
    std::vector<core::Summary> shown;
    std::vector<core::Summary> superseded;
    for (const core::Summary& summary : session.Memories(test::HULDA)) {
        (summary.superseded ? superseded : shown).push_back(summary);
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].fromTurn == 2);
    CHECK(shown[0].toTurn == 3);
    CHECK(shown[0].text == "Summary.");
    CHECK(shown[0].actorId == test::HULDA);
    CHECK(shown[0].conversationId.empty());
    REQUIRE(superseded.size() == 2);
    CHECK(superseded[0].conversationId == c2);
    CHECK(superseded[1].conversationId == c3);

    // Saadia's memory is untouched.
    const std::vector<core::Summary> saadias = session.Memories(test::SAADIA);
    REQUIRE(saadias.size() == 1);
    CHECK_FALSE(saadias[0].superseded);
    CHECK(saadias[0].conversationId == c1);
}
