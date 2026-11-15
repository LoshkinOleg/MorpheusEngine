#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "store/RunStore.h"
#include "store/Sqlite.h"
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

TEST_CASE("An NPC behind in a conversation is caught up before it speaks", "[hosted][memory]")
{
    // A fold during Hulda's turn leaves Saadia behind; her next turn writes her memory of those turns first. With a
    // 2500 context and HostedSettings' budgets L3 gets about 680 tokens: one line of 1600 bytes (400 tokens) fits, two
    // do not, and an echoed memory of one (about 410 tokens) stays within summary_cap 512.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE, NONE};
    script.lines = {std::string(1600, 'a'), std::string(1600, 'b'), "Short.", "Saadia here."};
    test::ScriptedBackend backend(2500, test::MemoryEchoResponder(script));
    core::Settings settings = test::HostedSettings();
    settings.contextSize = 2500;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_catchup"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "One."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Two."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Three."), {}, {}));
    const int folded = session.Conversation(id)->foldedThrough;
    REQUIRE(folded >= 1);
    CHECK(session.Memories(test::SAADIA).empty());

    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "Saadia?"), {}, {}));
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    REQUIRE_FALSE(saadia.empty());
    CHECK(saadia.back().toTurn <= folded);
    CHECK(saadia[0].text.find("aaaa") != std::string::npos); // She witnessed the first turn.
    // The catch-up ran before she spoke: her resolve prompt's tail holds that memory.
    const std::string& tail = session.LastAssembly().prompt.messages.back().content;
    CHECK_THAT(tail, ContainsSubstring("# What Saadia remembers"));
    CHECK_THAT(tail, ContainsSubstring("aaaa"));
}

TEST_CASE("A share made while the conversation is open waits, then applies once it ended and the teller caught up",
          "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"The well is poisoned."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    const std::filesystem::path file = test::HostedRunFile("hosted_share_pending");
    std::string id;
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Any news?"), {}, {}));

        const turn::ShareResult early = session.ShareMemories(test::HULDA, test::SAADIA, id);
        CHECK(early.pending);
        CHECK(early.shared == 0);
        // The same share again waits too, but is not stored twice.
        const turn::ShareResult again = session.ShareMemories(test::HULDA, test::SAADIA, id);
        CHECK(again.pending);
        CHECK(again.shared == 0);
        CHECK(session.Memories(test::SAADIA).empty());
    }
    CHECK(store::RunStore::Open(file).PendingShares().size() == 1);

    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
        session.EndConversation(id);
        REQUIRE(session.Maintain({}));
        const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
        REQUIRE(saadia.size() == 1);
        CHECK(saadia[0].toldBy == test::HULDA);
        CHECK(saadia[0].text.find("poisoned") != std::string::npos);
        // Applied once: another pass copies nothing more.
        REQUIRE(session.Maintain({}));
        CHECK(session.Memories(test::SAADIA).size() == 1);
    }
    // The applied share is no longer pending.
    CHECK(store::RunStore::Open(file).PendingShares().empty());
}

TEST_CASE("A share made after the conversation ended but before the teller caught up waits for Maintain",
          "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"The well is poisoned."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_share_ended_behind"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Any news?"), {}, {}));
    session.EndConversation(id);

    // Ended, but Hulda remembers none of it yet: the share waits.
    const turn::ShareResult early = session.ShareMemories(test::HULDA, test::SAADIA, id);
    CHECK(early.pending);
    CHECK(early.shared == 0);
    CHECK(session.Memories(test::SAADIA).empty());

    // Maintain catches her up, then applies it.
    REQUIRE(session.Maintain({}));
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    REQUIRE(saadia.size() == 1);
    CHECK(saadia[0].toldBy == test::HULDA);
    CHECK(saadia[0].text.find("poisoned") != std::string::npos);
}

TEST_CASE("Maintain stopped before a pending share leaves it pending; the next pass applies it", "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"The well is poisoned."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    const std::filesystem::path file = test::HostedRunFile("hosted_share_stopped");
    std::string id;
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Any news?"), {}, {}));
        session.EndConversation(id);
        REQUIRE(session.Maintain({}));
    }
    // A pending share whose teller's memory is already complete: only the share loop has work left.
    store::RunStore::Open(file).AddPendingShare({0, test::HULDA, test::SAADIA, id});

    turn::HostedSession session =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(session.Maintain(stopped.get_token()));
    CHECK(session.Memories(test::SAADIA).empty());
    REQUIRE(session.Maintain({}));
    CHECK(session.Memories(test::SAADIA).size() == 1);
}

TEST_CASE("Maintain refuses a pending share whose conversation is not in the run, as damaged saved data",
          "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    const std::filesystem::path file = test::HostedRunFile("hosted_share_damaged");
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
        const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    }
    // A pending_shares row naming a conversation the run does not have.
    const std::int64_t shareId = store::RunStore::Open(file).AddPendingShare({0, test::HULDA, test::SAADIA, "c9"});

    turn::HostedSession session =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
    CHECK_THROWS_AS(session.Maintain({}), store::StoreError);
    CHECK_THROWS_WITH(session.Maintain({}),
                      ContainsSubstring(std::format("pending share {}", shareId)) && ContainsSubstring("'c9'"));
}

TEST_CASE("A share refuses a teller who witnessed nothing, the same actor, the player, and an unknown conversation",
          "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_share_refused"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    CHECK_THROWS_WITH(session.ShareMemories(test::SAADIA, test::HULDA, id), ContainsSubstring("from:"));
    CHECK_THROWS_WITH(session.ShareMemories(test::AELA, test::HULDA, id), ContainsSubstring("from:")); // The player.
    CHECK_THROWS_WITH(session.ShareMemories(test::HULDA, test::HULDA, id), ContainsSubstring("to:"));
    CHECK_THROWS_WITH(session.ShareMemories(test::HULDA, "", id), ContainsSubstring("to:"));
    CHECK_THROWS_WITH(session.ShareMemories(test::HULDA, test::AELA, id),
                      ContainsSubstring("to:") && ContainsSubstring("is the player"));
    CHECK_THROWS_WITH(session.ShareMemories(test::HULDA, test::SAADIA, "c9"), ContainsSubstring("conversation_id"));
}

TEST_CASE("A share made after the teller condensed still copies its superseded summaries of the conversation",
          "[hosted][memory]")
{
    // Folds echo what they read; a condense writes "Condensed.". A line of 240 bytes puts Hulda's memories over
    // summary_cap 64 after the second conversation.
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"The well is poisoned.", std::string(240, 'x')};
    const test::ScriptedBackend::Responder echo = test::MemoryEchoResponder(script);
    test::ScriptedBackend backend(16384, [&echo](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        return prompt.messages.back().content.find("Condense what") != std::string::npos ? std::string("Condensed.")
                                                                                         : echo(prompt, spec);
    });
    core::Settings settings = test::HostedSettings();
    settings.budget.summaryCap = 64;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_share_after_condense"),
                                                            test::FixtureHostData(), settings, 42, backend);
    std::vector<std::string> ids;
    for (int i = 0; i < 2; ++i) {
        ids.push_back(session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation()));
        REQUIRE(session.RunTurn(test::Request(ids.back(), test::HULDA, "News?"), {}, {}));
        session.EndConversation(ids.back());
        REQUIRE(session.Maintain({}));
    }
    // Hulda's memory of the first conversation is superseded by her condensed one.
    const std::vector<core::Summary> hulda = session.Memories(test::HULDA);
    REQUIRE_FALSE(hulda.empty());
    const core::Summary original = hulda.front();
    REQUIRE(original.superseded);
    REQUIRE(original.conversationId == ids[0]);

    const turn::ShareResult result = session.ShareMemories(test::HULDA, test::SAADIA, ids[0]);
    CHECK(result.shared == 1);
    CHECK_FALSE(result.pending);
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    REQUIRE(saadia.size() == 1);
    CHECK(saadia[0].text == original.text);
    CHECK(saadia[0].sourceId == original.id);
    CHECK(saadia[0].toldBy == test::HULDA);
    CHECK_FALSE(saadia[0].superseded);
}

TEST_CASE("A shared memory is a snapshot: the teller's later condense leaves it as it was", "[hosted][memory]")
{
    // Folds echo what they read; a condense writes "Condensed." (an echo would stay over the small cap). A line of 240
    // bytes (a memory of about 65 tokens) puts Hulda's memories over summary_cap 64 after each later conversation.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"The well is poisoned.", std::string(240, 'x'), std::string(240, 'y')};
    const test::ScriptedBackend::Responder echo = test::MemoryEchoResponder(script);
    test::ScriptedBackend backend(16384, [&echo](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        return prompt.messages.back().content.find("Condense what") != std::string::npos ? std::string("Condensed.")
                                                                                         : echo(prompt, spec);
    });
    core::Settings settings = test::HostedSettings();
    settings.budget.summaryCap = 64;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_share_snapshot"),
                                                            test::FixtureHostData(), settings, 42, backend);

    // The first conversation, remembered by Hulda and shared to Saadia.
    const std::string first = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "News?"), {}, {}));
    session.EndConversation(first);
    REQUIRE(session.Maintain({}));
    REQUIRE(session.ShareMemories(test::HULDA, test::SAADIA, first).shared == 1);
    const core::Summary copy = session.Memories(test::SAADIA).at(0);

    // Two more conversations put Hulda's memories over summary_cap 64: the upkeep condenses them.
    for (int i = 0; i < 2; ++i) {
        const std::string next = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(next, test::HULDA, "More?"), {}, {}));
        session.EndConversation(next);
        REQUIRE(session.Maintain({}));
    }
    const std::vector<core::Summary> hulda = session.Memories(test::HULDA);
    const auto shown = std::count_if(hulda.begin(), hulda.end(), [](const core::Summary& s) { return !s.superseded; });
    CHECK(shown == 1);
    REQUIRE_FALSE(hulda.empty());
    CHECK(hulda.front().superseded); // Her first summary is kept, superseded.

    // Saadia's copy is as it was.
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    REQUIRE(saadia.size() == 1);
    CHECK(saadia[0].text == copy.text);
    CHECK(saadia[0].toldBy == test::HULDA);
    CHECK(saadia[0].sourceId == copy.sourceId);
}

TEST_CASE("A fold during a turn indexes its turns; a share and a catch-up with its condense index only memories",
          "[hosted][memory]")
{
    // Memory calls write "Summary.". With a 2500 context L3 gets about 680 tokens: two lines of 1600 bytes (400 tokens
    // each) do not fit, so Hulda's third turn of the second conversation folds. A summary_cap of 11 holds one
    // "Summary." (9 tokens) but not a memory told by Hulda next to it, so Saadia condenses when she catches up.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE, NONE};
    script.lines = {"Welcome.", std::string(1600, 'a'), std::string(1600, 'b'), "Short."};
    test::ScriptedBackend backend(2500, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = 2500;
    settings.budget.summaryCap = 11;
    const std::filesystem::path file = test::HostedRunFile("hosted_memory_sync_cost");
    turn::HostedSession session = turn::HostedSession::Open(file, test::FixtureHostData(), settings, 42, backend);
    // The turn vectors the run has stored, read through a second connection.
    auto turnVectors = [&file] {
        store::Database db = store::Database::Open(file, false);
        store::Statement count = db.Prepare("SELECT COUNT(*) FROM embeddings WHERE kind = 'turn'");
        REQUIRE(count.Step());
        return static_cast<int>(count.ColumnInt(0));
    };
    // Removes the stored turn vectors: any later sync of the turn index embeds every folded turn again.
    auto dropTurnVectors = [&file] {
        store::Database db = store::Database::Open(file, false);
        db.Exec("DELETE FROM embeddings WHERE kind = 'turn'");
    };

    // The first conversation, Hulda's alone, ended and folded: one turn vector, and Hulda's memory of it.
    const std::string first = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "Hello."), {}, {}));
    session.EndConversation(first);
    REQUIRE(session.Maintain({}));
    REQUIRE(turnVectors() == 1);

    // The second, with Saadia listening: the fold during Hulda's third turn indexes the turns it folded.
    const std::string second =
        session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(second, test::HULDA, "One."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(second, test::HULDA, "Two."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(second, test::HULDA, "Three."), {}, {}));
    const int foldedThrough = session.Conversation(second)->foldedThrough;
    const std::vector<core::HostedTurn> turns = session.Turns(second);
    const auto folded = std::count_if(turns.begin(), turns.end(), [foldedThrough](const core::HostedTurn& turn) {
        return turn.number <= foldedThrough;
    });
    REQUIRE(folded >= 1);
    CHECK(turnVectors() == 1 + folded);
    REQUIRE(session.Memories(test::SAADIA).empty()); // Saadia is behind.

    // Hulda tells Saadia what she remembers of the first conversation: only the copy is embedded.
    dropTurnVectors();
    int embedded = backend.EmbeddedTexts();
    const turn::ShareResult shared = session.ShareMemories(test::HULDA, test::SAADIA, first);
    REQUIRE(shared.shared == 1);
    CHECK(backend.EmbeddedTexts() == embedded + 1);
    CHECK(turnVectors() == 0);

    // Saadia catches up on the second conversation, condensing on the way: only her new memories are embedded.
    dropTurnVectors();
    embedded = backend.EmbeddedTexts();
    REQUIRE(session.Maintain({}));
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    CHECK(std::any_of(saadia.begin(), saadia.end(), [](const core::Summary& memory) { return memory.superseded; }));
    CHECK(std::any_of(saadia.begin(), saadia.end(),
                      [&second](const core::Summary& memory) { return memory.conversationId == second; }));
    CHECK(backend.EmbeddedTexts() == embedded + static_cast<int>(saadia.size()) - 1); // All but the copy are new.
    CHECK(turnVectors() == 0);
}
