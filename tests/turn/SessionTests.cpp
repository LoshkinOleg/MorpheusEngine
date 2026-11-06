#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "SceneWorld.h"
#include "TestPaths.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/ResolveGrammar.h"
#include "turn/Review.h"
#include "turn/Session.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

const std::string MOVE = R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks there"}]})";
const std::string CLIMB = R"({"intent":"climb","checks":[{"stat":"agility","difficulty":"hard","stakes":"a fall",)"
                          R"("on_success":[],"on_failure":[{"op":"add","path":"player.health","value":-2,"reason":"falls"}]}],"changes":[]})";

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

core::Settings TestSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf"; // Only named in the run meta; never opened.
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingQueryPrefix = "search_query: ";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {256, 256, 256, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

// Answers the way the model would, by call: resolve calls (their grammar has "intent") get `resolve`, summary calls
// get "Summary.", and everything else is narration.
test::ScriptedBackend::Responder Responder(std::string resolve, std::string narration = "You walk on.")
{
    return [resolve, narration](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("intent") != std::string::npos) {
            return resolve;
        }
        const std::string& last = prompt.messages.back().content;
        if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
            return std::string("Summary.");
        }
        return narration;
    };
}

fs::path NewRunFile(const std::string& name)
{
    return test::FreshTempDir(name) / "run.db";
}

// For a context of 3000 with 2000-byte narrations. FakeBackend counts 1 token per 4 bytes: L0+L1 is about 477
// tokens, so L3 gets about 1255, while each turn adds about 520. Two turns fit the transcript, three do not.
core::Settings FoldingSettings()
{
    core::Settings settings = TestSettings();
    settings.budget = {64, 600, 200, 400, 1024, 1024, 512, 100};
    return settings;
}

// FoldingSettings with room in the tail for one history snippet (about 160 tokens): the tail allowance and the
// retrieval cap grow by 200, so a context of 3200 keeps the transcript budget near 1255.
core::Settings RetrievingFoldingSettings()
{
    core::Settings settings = FoldingSettings();
    settings.budget.tailAllowance += 200;
    settings.budget.retrievalCap += 200;
    return settings;
}

// The resolve prompt's tail (its last message): state, retrieved snippets, input and instruction.
std::string Tail(const inference::Prompt& prompt)
{
    return prompt.messages.back().content;
}

// A review that finds nothing wrong, with the turn's digest.
const std::string CONSISTENT = R"({"consistent":true,"problems":[],"digest":"The player walked to the lighthouse."})";

// TestSettings with the review call on: the narration check and the digest as given, and the review's budget from
// settings.example.json.
core::Settings ReviewSettings(bool check, bool digest)
{
    core::Settings settings = TestSettings();
    settings.narrationCheck = check;
    settings.compactTranscript = digest;
    settings.budget.reviewMaxTokens = 384;
    return settings;
}

// A scripted model for review tests. Resolve calls get `resolve`; review calls (their system message is REVIEW_RULES)
// get `reviews` in order; summary calls get "Summary."; narrate calls get `narrations` in order. Running out of
// replies is a test bug. The script must outlive the backends that call it.
struct ReviewScript {
    std::string resolve = {};
    std::vector<std::string> narrations = {};
    std::vector<std::string> reviews = {};
    std::size_t narrated = 0;
    std::size_t reviewed = 0;

    test::ScriptedBackend::Responder Responder()
    {
        return [this](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
            if (spec.grammar.find("intent") != std::string::npos) {
                return resolve;
            }
            if (prompt.messages.front().content == turn::REVIEW_RULES) {
                MORPHEUS_REQUIRE(reviewed < reviews.size(), "ReviewScript: no review reply left");
                return reviews[reviewed++];
            }
            const std::string& last = prompt.messages.back().content;
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
                return "Summary.";
            }
            MORPHEUS_REQUIRE(narrated < narrations.size(), "ReviewScript: no narration left");
            return narrations[narrated++];
        };
    }
};

} // namespace

TEST_CASE("A turn resolves, applies, narrates and commits", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk to the lighthouse."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_turn"), 42, backend);

    std::vector<std::string> events;
    turn::TurnCallbacks callbacks;
    callbacks.onResolved = [&](const core::TurnRecord& record) {
        events.push_back("resolved " + record.applied.at(0).change.path);
    };
    callbacks.onNarrationToken = [&](std::string_view) {
        if (events.back() != "token") {
            events.push_back("token");
        }
    };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->number == 1);
    CHECK(record->attempt == 0);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(events == std::vector<std::string>{"resolved player.location", "token"});
    CHECK(record->timings.contains("resolve"));
    CHECK(record->timings.contains("narrate"));
    CHECK(session.State().at("player").at("fields").at("location") == "lighthouse");
    CHECK(session.History().size() == 1);
    CHECK(session.Meta().modelId == "chat");
}

TEST_CASE("Resolve and narrate think within their budgets, and the thinking stays in the telemetry", "[session]")
{
    core::Settings settings = TestSettings();
    settings.budget.resolveThinkingTokens = 512;
    settings.budget.narrateThinkingTokens = 384;
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk to the lighthouse."));
    backend.SetThinker([](const inference::Prompt&, const inference::GenerationSpec& spec) {
        return spec.grammar.empty() ? std::string("Keep the prose short.") : std::string("Walking is safe.");
    });
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_thinking"), 42, backend);

    std::string streamed;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationToken = [&](std::string_view piece) { streamed += piece; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);

    // Each call gets its own budget.
    const auto& calls = backend.Calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].spec.thinkingMaxTokens == 512);
    CHECK(calls[1].spec.thinkingMaxTokens == 384);

    // The thinking reaches the telemetry only: not the narration, the stream or the narrate prompt.
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(streamed == "You walk to the lighthouse.");
    for (const inference::Message& message : calls[1].prompt.messages) {
        CHECK_THAT(message.content, !ContainsSubstring("Walking is safe."));
    }
    const nlohmann::json& resolveThinking = record->timings.at("resolve").at("thinking");
    CHECK(resolveThinking.at("text") == "Walking is safe.");
    CHECK(resolveThinking.at("tokens") == backend.CountTokens("Walking is safe."));
    CHECK(resolveThinking.at("stopped_at_limit") == false);
    CHECK(record->timings.at("narrate").at("thinking").at("text") == "Keep the prose short.");

    // A history reload brings the thinking back with the turn.
    CHECK(session.History().back().timings.at("narrate").at("thinking").at("text") == "Keep the prose short.");
}

TEST_CASE("Calls without a thinking budget do not think", "[session]")
{
    core::Settings settings = TestSettings();
    settings.budget.resolveThinkingTokens = 512; // Narrate keeps a budget of 0.
    test::ScriptedBackend backend(16384, Responder(MOVE));
    backend.SetThinker([](const inference::Prompt&, const inference::GenerationSpec&) { return std::string("Hmm."); });
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_thinking_off"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);

    CHECK(backend.Calls()[1].spec.thinkingMaxTokens == 0);
    CHECK(record->timings.at("resolve").contains("thinking"));
    CHECK_FALSE(record->timings.at("narrate").contains("thinking"));
}

TEST_CASE("Thinking budgets fail the turn on a model without a thinking mode", "[session]")
{
    core::Settings settings = TestSettings();
    settings.budget.resolveThinkingTokens = 512;
    test::ScriptedBackend backend(16384, Responder(MOVE)); // No thinker: not a thinking model.
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_no_thinking_mode"), 42, backend);
    CHECK_THROWS_AS(session.RunTurn("I walk to the lighthouse.", {}, {}), inference::InferenceError);
}

TEST_CASE("The narrate prompt extends the resolve prompt", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_prompts"), 42, backend);
    REQUIRE(session.RunTurn("I walk to the lighthouse.", {}, {}));

    const auto& calls = backend.Calls();
    REQUIRE(calls.size() == 2);
    const inference::Prompt& resolve = calls[0].prompt;
    const inference::Prompt& narrate = calls[1].prompt;
    CHECK_THAT(calls[0].spec.grammar, ContainsSubstring("root ::="));
    CHECK(calls[1].spec.grammar.empty());
    CHECK_THAT(resolve.messages.front().content, ContainsSubstring("# Engine"));
    CHECK_THAT(resolve.messages.front().content, ContainsSubstring("# Game reference"));
    CHECK_THAT(resolve.messages.back().content, ContainsSubstring(std::string(turn::RESOLVE_INSTRUCTION)));
    REQUIRE(narrate.messages.size() == resolve.messages.size() + 2);
    for (std::size_t i = 0; i < resolve.messages.size(); ++i) {
        CHECK(narrate.messages[i].content == resolve.messages[i].content);
    }
    CHECK(narrate.messages[resolve.messages.size()].content == MOVE);
    CHECK_THAT(narrate.messages.back().content, ContainsSubstring("player.location = \"lighthouse\""));
}

TEST_CASE("Unreadable resolve output still narrates, with nothing changed", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(R"({"intent":"walk","checks":[)", "You hesitate."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_unreadable"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk.", {}, {});
    REQUIRE(record);
    CHECK(record->applied.empty());
    CHECK(record->resolve.contains("unparsed"));
    CHECK(record->narration == "You hesitate.");
    CHECK_THAT(backend.Calls().at(1).prompt.messages.back().content, ContainsSubstring("could not be resolved"));
    CHECK(session.State() == MiniWorld().initialState);
}

TEST_CASE("The resolve telemetry records why the output could not be read", "[session]")
{
    SECTION("cut off")
    {
        test::ScriptedBackend backend(16384, Responder(R"({"intent":"walk","checks":[)"));
        turn::Session session =
            turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_resolve_error_cut"), 42, backend);
        const std::optional<core::TurnRecord> record = session.RunTurn("I walk.", {}, {});
        REQUIRE(record);
        CHECK_FALSE(record->timings.at("resolve").value("error", std::string()).empty());
    }
    SECTION("of the wrong shape")
    {
        // Valid JSON, so the record keeps it as it is: only the telemetry says why nothing was applied.
        test::ScriptedBackend backend(16384, Responder(R"({"intent":"walk","checks":"none","changes":[]})"));
        turn::Session session =
            turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_resolve_error_shape"), 42, backend);
        const std::optional<core::TurnRecord> record = session.RunTurn("I walk.", {}, {});
        REQUIRE(record);
        CHECK_FALSE(record->resolve.contains("unparsed"));
        CHECK_FALSE(record->timings.at("resolve").value("error", std::string()).empty());
    }
    SECTION("but not for readable output")
    {
        test::ScriptedBackend backend(16384, Responder(MOVE));
        turn::Session session =
            turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_resolve_no_error"), 42, backend);
        const std::optional<core::TurnRecord> record = session.RunTurn("I walk.", {}, {});
        REQUIRE(record);
        CHECK_FALSE(record->timings.at("resolve").contains("error"));
    }
}

TEST_CASE("Cancelling a turn commits nothing", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk a long way."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_cancel"), 42, backend);
    std::stop_source stop;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationToken = [&](std::string_view) { stop.request_stop(); };
    CHECK_FALSE(session.RunTurn("I walk.", callbacks, stop.get_token()));
    CHECK(session.History().empty());
    CHECK(session.State() == MiniWorld().initialState);
}

TEST_CASE("Undo removes the latest turn", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_undo"), 42, backend);
    CHECK_THROWS_AS(session.Undo(), turn::TurnError);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    session.Undo();
    CHECK(session.History().empty());
    CHECK(session.State() == MiniWorld().initialState);
}

TEST_CASE("Regenerate keeps the rolls and samples new text", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(CLIMB, "You climb."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_regenerate"), 42, backend);
    CHECK_THROWS_AS(session.Regenerate({}, {}), turn::TurnError);
    REQUIRE(session.RunTurn("I climb the cliff.", {}, {}));
    const core::TurnRecord first = session.History().at(0);

    const std::optional<core::TurnRecord> second = session.Regenerate({}, {});
    REQUIRE(second);
    CHECK(second->number == 1);
    CHECK(second->attempt == 1);
    CHECK(second->input == "I climb the cliff.");
    REQUIRE(second->checks.size() == 1);
    CHECK(second->checks[0].result.rolls == first.checks[0].result.rolls);
    const auto& calls = backend.Calls(); // resolve, narrate, resolve, narrate
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].spec.sampling.seed != calls[0].spec.sampling.seed);
    CHECK(calls[3].spec.sampling.seed != calls[1].spec.sampling.seed);
    CHECK(session.History().size() == 1);

    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(session.Regenerate({}, stop.get_token()));
    REQUIRE(session.History().size() == 1);
    CHECK(session.History()[0].attempt == 1); // The cancelled attempt put the previous turn back.
}

TEST_CASE("Folding keeps every prompt within the context", "[session]")
{
    // Two turns fit the transcript and three do not (FoldingSettings), so folding must start by the third turn.
    test::ScriptedBackend backend(3000, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session =
        turn::Session::Start(MiniWorld(), FoldingSettings(), NewRunFile("session_folding"), 42, backend);

    for (int i = 0; i < 6; ++i) {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
        CHECK(session.LastAssembly().Fits());
        REQUIRE(session.Maintain({}));
    }
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE_FALSE(summaries.empty());
    CHECK(summaries.front().fromTurn == 1);
    CHECK_THAT(session.LastAssembly().prompt.messages.front().content, ContainsSubstring("# Story so far"));
    CHECK(session.LastAssembly().prompt.messages.size() < 2 + 2 * 6);
}

TEST_CASE("Load reopens a run and checks it against the world", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    const fs::path file = NewRunFile("session_load");
    {
        turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), file, 42, backend);
        REQUIRE(session.RunTurn("I walk.", {}, {}));
    }
    turn::Session loaded = turn::Session::Load(MiniWorld(), TestSettings(), file, backend);
    CHECK_FALSE(loaded.WorldChanged());
    CHECK(loaded.History().size() == 1);
    CHECK(loaded.State().at("player").at("fields").at("location") == "lighthouse");

    // The same world with an edited rules.md opens, flagged.
    const fs::path editedFolder = test::FreshTempDir("session_load_edited") / "mini";
    fs::copy(test::TestDataDir() / "worlds" / "mini", editedFolder, fs::copy_options::recursive);
    test::WriteFile(editedFolder / "rules.md", "You are the GM of an edited test world.\n");
    CHECK(turn::Session::Load(core::LoadWorld(editedFolder), TestSettings(), file, backend).WorldChanged());

    const core::World other = core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");
    CHECK_THROWS_AS(turn::Session::Load(other, TestSettings(), file, backend), core::DataError);
}

TEST_CASE("Player input is cleaned and must not be empty", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_input"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("\x01  I walk.\x1E \n", {}, {});
    REQUIRE(record);
    CHECK(record->input == "I walk.");
    CHECK_THROWS_AS(session.RunTurn(" \x02 ", {}, {}), turn::TurnError);
}

TEST_CASE("Load refuses a run whose latest state no longer fits the world's schema", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    const fs::path file = NewRunFile("session_load_schema");
    {
        turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), file, 42, backend);
        REQUIRE(session.RunTurn("I walk.", {}, {})); // location becomes "lighthouse".
    }

    // The location becomes an enum without "lighthouse": the initial state still fits, the stored state does not.
    const fs::path editedFolder = test::FreshTempDir("session_load_schema_world") / "mini";
    fs::copy(test::TestDataDir() / "worlds" / "mini", editedFolder, fs::copy_options::recursive);
    test::WriteFile(editedFolder / "state_schema.json", R"({
  "entity_types": {
    "character": {
      "fields": {
        "health": { "type": "int", "min": 0, "max": 10 },
        "agility": { "type": "int", "min": 0, "max": 5 },
        "location": { "type": "enum", "values": ["shore"] },
        "inventory": { "type": "counter" }
      }
    }
  }
}
)");
    CHECK_THROWS_AS(turn::Session::Load(core::LoadWorld(editedFolder), TestSettings(), file, backend), core::DataError);
}

TEST_CASE("Regenerate puts the previous turn back when the new attempt fails", "[session]")
{
    int resolves = 0;
    test::ScriptedBackend backend(16384, [&resolves](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("intent") != std::string::npos && ++resolves == 2) {
            throw inference::InferenceError("the model stopped responding");
        }
        return Responder(MOVE, "You walk to the lighthouse.")(prompt, spec);
    });
    turn::Session session =
        turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_regenerate_fail"), 42, backend);
    REQUIRE(session.RunTurn("I walk to the lighthouse.", {}, {}));

    CHECK_THROWS_AS(session.Regenerate({}, {}), inference::InferenceError);
    REQUIRE(session.History().size() == 1);
    CHECK(session.History()[0].attempt == 0);
    CHECK(session.History()[0].narration == "You walk to the lighthouse.");
    CHECK(session.State().at("player").at("fields").at("location") == "lighthouse");
}

TEST_CASE("Undo takes undone turns out of retrieval", "[session]")
{
    test::ScriptedBackend backend(3200, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session =
        turn::Session::Start(MiniWorld(), RetrievingFoldingSettings(), NewRunFile("session_undo_index"), 42, backend);

    // Three turns, then Maintain folds turn 1, which becomes searchable history: the next turn retrieves it.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
    }
    REQUIRE(session.Maintain({}));
    REQUIRE(session.Summaries().size() == 1);
    REQUIRE(session.Summaries()[0].toTurn == 1);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    REQUIRE_THAT(Tail(session.LastAssembly().prompt), ContainsSubstring("Turn 1:"));

    // Undoing below the summary deletes it and turn 1: the new turn 1 must not retrieve the undone one.
    for (int i = 0; i < 4; ++i) {
        session.Undo();
    }
    REQUIRE(session.Summaries().empty());
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK_THAT(Tail(session.LastAssembly().prompt), !ContainsSubstring("Turn 1:"));
}

TEST_CASE("A cancelled regenerate keeps the summary that covers the latest turn", "[session]")
{
    // A 6000-byte narration (about 1500 tokens) is over the transcript budget on its own, so Maintain folds turn 1.
    test::ScriptedBackend backend(3200, Responder(MOVE, std::string(6000, 'w')));
    turn::Session session = turn::Session::Start(MiniWorld(), RetrievingFoldingSettings(),
                                                 NewRunFile("session_regenerate_fold"), 42, backend);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    REQUIRE(session.Maintain({}));
    REQUIRE(session.Summaries().size() == 1);
    REQUIRE(session.Summaries()[0].toTurn == 1);

    // The attempt is cancelled at its resolve call, whose prompt must not retrieve the attempt it replaces.
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(session.Regenerate({}, stop.get_token()));
    const auto& calls = backend.Calls();
    REQUIRE_FALSE(calls.empty());
    REQUIRE_THAT(calls.back().spec.grammar, ContainsSubstring("intent"));
    CHECK_THAT(Tail(calls.back().prompt), !ContainsSubstring("Turn 1:"));

    // Turn 1 is back, with its summary, and searchable again.
    REQUIRE(session.History().size() == 1);
    CHECK(session.History()[0].attempt == 0);
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 1);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK_THAT(Tail(session.LastAssembly().prompt), ContainsSubstring("Turn 1:"));
}

TEST_CASE("A summary_cap too small for a condensed summary throws instead of condensing forever", "[session]")
{
    // "# Story so far\nTurns 1-1: Summary.\n" is 9 tokens, over a cap of 8 even after condensing.
    core::Settings settings = FoldingSettings();
    settings.budget.summaryCap = 8;
    // A regression would condense forever: past a call limit the responder throws, so the checks below fail.
    int calls = 0;
    test::ScriptedBackend backend(3000, [&calls](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (++calls > 50) {
            throw std::runtime_error("runaway: more than 50 model calls");
        }
        return Responder(MOVE, std::string(2000, 'w'))(prompt, spec);
    });
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_summary_cap"), 42, backend);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
    }

    SECTION("in Maintain")
    {
        CHECK_THROWS_AS(session.Maintain({}), core::DataError);
    }
    SECTION("in the turn")
    {
        CHECK_THROWS_AS(session.RunTurn("I walk.", {}, {}), core::DataError);
        CHECK(session.History().size() == 3);
    }
}

TEST_CASE("A turn folds the transcript itself when it is over budget", "[session]")
{
    test::ScriptedBackend backend(3000, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session =
        turn::Session::Start(MiniWorld(), FoldingSettings(), NewRunFile("session_turn_folds"), 42, backend);

    // No Maintain between turns: from the fourth turn on, the turn's own fit loop has to fold.
    for (int i = 0; i < 6; ++i) {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
        CHECK(session.LastAssembly().Fits());
    }
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE_FALSE(summaries.empty());
    CHECK(summaries.front().fromTurn == 1);
}

TEST_CASE("A turn condenses a summary that is over its cap", "[session]")
{
    test::ScriptedBackend backend(3000, Responder(MOVE, std::string(2000, 'w')));
    const fs::path file = NewRunFile("session_turn_condenses");
    {
        // Each Maintain from the third turn on folds one turn into a summary of its own.
        turn::Session session = turn::Session::Start(MiniWorld(), FoldingSettings(), file, 42, backend);
        for (int i = 0; i < 6; ++i) {
            REQUIRE(session.RunTurn("I walk.", {}, {}));
            REQUIRE(session.Maintain({}));
        }
        REQUIRE(session.Summaries().size() >= 3);
    }

    // Three or more summaries are at least 19 tokens, over a cap of 16; condensed into one they are 9.
    core::Settings smallCap = FoldingSettings();
    smallCap.budget.summaryCap = 16;
    turn::Session session = turn::Session::Load(MiniWorld(), smallCap, file, backend);
    const std::size_t callsBefore = backend.Calls().size();
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK(session.LastAssembly().Fits());
    CHECK(session.Summaries().size() == 1);
    bool condensed = false;
    for (std::size_t i = callsBefore; i < backend.Calls().size(); ++i) {
        condensed = condensed || backend.Calls()[i].prompt.messages.back().content == turn::CONDENSE_INSTRUCTION;
    }
    CHECK(condensed);
}

TEST_CASE("An input whose tail is over tail_allowance is a TurnError", "[session]")
{
    core::Settings settings = TestSettings();
    settings.budget.tailAllowance = 300;
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_tail_error"), 42, backend);
    CHECK_THROWS_AS(session.RunTurn(std::string(2000, 'x'), {}, {}), turn::TurnError);
    CHECK(backend.Calls().empty());
    CHECK(session.History().empty());
}

TEST_CASE("A prompt that overflows the context sheds retrieval hits as a last resort", "[session]")
{
    const std::string input = "I ask the keeper about the storm.";

    // A roomy run measures the prompt with its lore hits.
    test::ScriptedBackend roomyBackend(16384, Responder(MOVE));
    turn::Session roomy =
        turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_shed_roomy"), 42, roomyBackend);
    const std::optional<core::TurnRecord> roomyRecord = roomy.RunTurn(input, {}, {});
    REQUIRE(roomyRecord);
    REQUIRE_FALSE(roomyRecord->timings.at("retrieval").empty());
    const context::Assembly measured = roomy.LastAssembly();

    // The tight run's tail allowance is exactly that tail, so the tail is not over budget, and its context is one
    // token short of the prompt plus the narrate message and both generations (FakeBackend: 4 template tokens per
    // message), so the prompt overflows with every hit. There is no transcript to fold: only shedding hits helps.
    core::Settings settings = TestSettings();
    settings.budget.tailAllowance = measured.tokens.tail;
    const core::BudgetSettings& budget = settings.budget;
    const int contextSize = measured.tokens.total + budget.narrateAllowance + 4 + 4 + budget.resolveMaxTokens +
                            budget.narrateMaxTokens - 1;
    test::ScriptedBackend tightBackend(contextSize, Responder(MOVE));
    turn::Session tight =
        turn::Session::Start(MiniWorld(), settings, NewRunFile("session_shed_tight"), 42, tightBackend);
    const std::optional<core::TurnRecord> tightRecord = tight.RunTurn(input, {}, {});
    REQUIRE(tightRecord);
    CHECK(tight.LastAssembly().Fits());
    CHECK(tightRecord->timings.at("retrieval").size() < roomyRecord->timings.at("retrieval").size());
}

TEST_CASE("A very long input is cut for the retrieval query", "[session]")
{
    core::Settings settings = TestSettings();
    settings.budget.tailAllowance = 2048;
    test::ScriptedBackend backend(16384, Responder(MOVE));
    backend.SetEmbedLimit(turn::EMBED_TEXT_CHARS); // An embedding context that holds no more than that.
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_long_input"), 42, backend);
    std::string input;
    while (input.size() <= turn::EMBED_TEXT_CHARS) {
        input += "I ask the keeper about the storm. ";
    }
    const std::optional<core::TurnRecord> record = session.RunTurn(input, {}, {});
    REQUIRE(record);
    CHECK(record->input.size() > turn::EMBED_TEXT_CHARS);
}

TEST_CASE("Assisted mode looks things up before resolving and keeps notes", "[session]")
{
    core::Settings settings = TestSettings();
    settings.memoryMode = core::MemoryMode::Assisted;
    const std::string listen =
        R"({"intent":"listen","checks":[],"changes":[],"notes":["The keeper hums an old sea shanty."]})";
    test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("lookups") != std::string::npos) {
            return std::string(R"({"lookups":[{"source":"lore","query":"keeper"}]})");
        }
        return Responder(listen, "The keeper hums.")(prompt, spec);
    });
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_assisted"), 42, backend);

    const std::optional<core::TurnRecord> record = session.RunTurn("I listen.", {}, {});
    REQUIRE(record);
    const auto& calls = backend.Calls(); // lookup, resolve, narrate
    REQUIRE(calls.size() == 3);
    CHECK_THAT(calls[0].prompt.messages.back().content, ContainsSubstring(std::string(turn::LOOKUP_INSTRUCTION)));
    CHECK_THAT(calls[1].prompt.messages.back().content, ContainsSubstring("keeper: The keeper says"));
    CHECK_THAT(calls[1].prompt.messages.front().content, ContainsSubstring("# Memory"));
    CHECK_THAT(calls[1].spec.grammar, ContainsSubstring("notes ::="));
    CHECK(record->notes == std::vector<std::string>{"The keeper hums an old sea shanty."});
    REQUIRE(record->timings.contains("lookup"));
    // The best match comes first. The fake embedding also lets a weaker "storm" hit through the score floor.
    CHECK(record->timings.at("lookup").at("requests").at(0).at("added").at(0) == "keeper");
    CHECK(record->timings.at("lookup").at("requests").at(0).at("kept").at(0) == "keeper"); // It reached the prompt.

    // After maintenance the note is searchable memory for later turns.
    REQUIRE(session.Maintain({}));
    REQUIRE(session.RunTurn("What shanty does the keeper hum?", {}, {}));
    CHECK_THAT(backend.Calls().at(4).prompt.messages.back().content,
               ContainsSubstring("Note from turn 1: The keeper hums an old sea shanty."));
}

TEST_CASE("A run loaded with a much smaller context folds what fits and stays playable", "[session]")
{
    // Twelve turns of about 520 tokens each are played in a roomy context, where nothing folds. At a context of
    // 3000 that transcript is about twice the context: half of it would not fit one fold call.
    const fs::path file = NewRunFile("session_shrunk_context");
    test::ScriptedBackend roomyBackend(16384, Responder(MOVE, std::string(2000, 'w')));
    {
        turn::Session session = turn::Session::Start(MiniWorld(), FoldingSettings(), file, 42, roomyBackend);
        for (int i = 0; i < 12; ++i) {
            REQUIRE(session.RunTurn("I walk.", {}, {}));
            REQUIRE(session.Maintain({}));
        }
        REQUIRE(session.Summaries().empty());
    }

    // The small backend throws InferenceError for any call whose prompt plus max_tokens is over its context.
    test::ScriptedBackend smallBackend(3000, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session = turn::Session::Load(MiniWorld(), FoldingSettings(), file, smallBackend);
    SECTION("the next turn folds")
    {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
        CHECK(session.LastAssembly().Fits());
        REQUIRE(session.Maintain({}));
    }
    SECTION("maintenance folds")
    {
        REQUIRE(session.Maintain({}));
        REQUIRE(session.RunTurn("I walk.", {}, {}));
        CHECK(session.LastAssembly().Fits());
    }
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE(summaries.size() >= 2); // No single fold call could take the oldest half.
    CHECK(summaries.front().fromTurn == 1);
    CHECK(summaries.front().text == "Summary.");
    REQUIRE(session.RunTurn("I walk.", {}, {}));
}

TEST_CASE("A turn too long to fold with a model call gets the engine's summary", "[session]")
{
    // Turn 1's narration (about 3000 tokens) is as long as the small context itself.
    const fs::path file = NewRunFile("session_fold_fallback");
    int narrations = 0;
    auto responder = [&narrations](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        const bool narrate = spec.grammar.empty() && prompt.messages.back().content != turn::SUMMARIZE_INSTRUCTION &&
                             prompt.messages.back().content != turn::CONDENSE_INSTRUCTION;
        const std::string narration = narrate && ++narrations == 1 ? std::string(12000, 'w') : std::string(2000, 'w');
        return Responder(MOVE, narration)(prompt, spec);
    };
    test::ScriptedBackend roomyBackend(16384, responder);
    {
        core::Settings roomy = FoldingSettings();
        roomy.budget.narrateMaxTokens = 3200; // So the long narration is within what the model may write.
        turn::Session session = turn::Session::Start(MiniWorld(), roomy, file, 42, roomyBackend);
        for (int i = 0; i < 3; ++i) {
            REQUIRE(session.RunTurn("I walk.", {}, {}));
        }
    }

    test::ScriptedBackend smallBackend(3000, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session = turn::Session::Load(MiniWorld(), FoldingSettings(), file, smallBackend);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK(session.LastAssembly().Fits());
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE_FALSE(summaries.empty());
    CHECK(summaries.front().fromTurn == 1);
    CHECK(summaries.front().toTurn == 1);
    CHECK(summaries.front().text == "Turn 1: I walk. [player.location = \"lighthouse\"]");
    REQUIRE(session.Maintain({}));
}

TEST_CASE("A summary too big to condense with a model call gets the engine's condensed summary", "[session]")
{
    // A run whose summaries were written under a much larger summary_cap: together about 3000 tokens, as large as
    // the small context, so the condense prompt (the system message holds them) cannot fit.
    const fs::path file = NewRunFile("session_condense_fallback");
    test::ScriptedBackend roomyBackend(16384, Responder(MOVE));
    {
        turn::Session session = turn::Session::Start(MiniWorld(), FoldingSettings(), file, 42, roomyBackend);
        for (int i = 0; i < 3; ++i) {
            REQUIRE(session.RunTurn("I walk.", {}, {}));
        }
    }
    {
        store::RunStore store = store::RunStore::Open(file);
        store.AddSummary({1, 1, std::string(6000, 's')});
        store.AddSummary({2, 2, std::string(6000, 't')});
    }

    test::ScriptedBackend smallBackend(3000, Responder(MOVE));
    turn::Session session = turn::Session::Load(MiniWorld(), FoldingSettings(), file, smallBackend);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK(session.LastAssembly().Fits());
    for (const test::ScriptedBackend::Call& call : smallBackend.Calls()) {
        CHECK(call.prompt.messages.back().content != std::string(turn::CONDENSE_INSTRUCTION));
    }
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text.starts_with("Turns 1-1: sss"));
    REQUIRE(session.Maintain({}));
}

TEST_CASE("Undoing below a condensed summary puts back a long transcript that still plays", "[session]")
{
    // A summary_cap of 16 condenses every few folds, so one summary soon covers turn 1 to K. Undoing below K deletes
    // it and puts turns 1 to K-1 back into the transcript: more than half the context of 3000 when K is 11 or more.
    core::Settings settings = FoldingSettings();
    settings.budget.summaryCap = 16;
    test::ScriptedBackend backend(3000, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_undo_condensed"), 42, backend);
    for (int i = 0; i < 16; ++i) {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
        REQUIRE(session.Maintain({}));
    }
    const int condensedThrough = session.Summaries().front().toTurn;
    REQUIRE(condensedThrough >= 11);
    while (static_cast<int>(session.History().size()) >= condensedThrough) {
        session.Undo();
    }
    REQUIRE(session.Summaries().empty());

    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK(session.LastAssembly().Fits());
    REQUIRE(session.Maintain({}));
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK(session.LastAssembly().Fits());
}

TEST_CASE("Assisted-mode lookups share retrieval_cap with the automatic hits", "[session]")
{
    // The automatic hit's line, "- storm: Storms arrive at dusk." and a newline, costs 8 tokens of a cap of 15, so
    // the looked-up keeper entry (12 tokens) no longer fits.
    core::Settings settings = TestSettings();
    settings.memoryMode = core::MemoryMode::Assisted;
    settings.budget.retrievalCap = 15;
    const std::string listen = R"({"intent":"listen","checks":[],"changes":[],"notes":[]})";
    test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("lookups") != std::string::npos) {
            return std::string(R"({"lookups":[{"source":"lore","query":"keeper"}]})");
        }
        return Responder(listen, "Dusk comes.")(prompt, spec);
    });
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_lookup_cap"), 42, backend);

    const std::optional<core::TurnRecord> record = session.RunTurn("Do storms arrive at dusk?", {}, {});
    REQUIRE(record);
    REQUIRE(record->timings.at("retrieval").size() == 1);
    CHECK(record->timings.at("retrieval").at(0).at("ref") == "storm");
    const nlohmann::json& request = record->timings.at("lookup").at("requests").at(0);
    CHECK(request.at("added").empty());
    CHECK(request.at("kept").empty());
    CHECK_THAT(Tail(session.LastAssembly().prompt), !ContainsSubstring("The keeper says"));
}

TEST_CASE("Engine mode makes no lookup call and keeps no notes", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(R"({"intent":"listen","checks":[],"changes":[]})"));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_engine_mode"), 42, backend);
    REQUIRE(session.RunTurn("I listen.", {}, {}));
    CHECK(backend.Calls().size() == 2);
    CHECK_THAT(backend.Calls().at(0).prompt.messages.front().content, !ContainsSubstring("# Memory"));
}

TEST_CASE("A consistent review commits the narration and its digest", "[session][review]")
{
    ReviewScript script{MOVE, {"You walk to the lighthouse."}, {CONSISTENT}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_review_consistent"), 42, backend);
    int restarts = 0;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { ++restarts; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(record->digest == "The player walked to the lighthouse.");
    CHECK(session.History().at(0).digest == "The player walked to the lighthouse."); // Stored with the turn.
    CHECK(restarts == 0);

    // The review is a standalone call: the rules, then the input, the outcome and the narration.
    const auto& calls = backend.Calls(); // resolve, narrate, review
    REQUIRE(calls.size() == 3);
    const inference::Prompt& review = calls[2].prompt;
    REQUIRE(review.messages.size() == 2);
    CHECK(review.messages[0].content == std::string(turn::REVIEW_RULES));
    CHECK_THAT(review.messages[1].content, ContainsSubstring("# Player\nI walk to the lighthouse.\n"));
    CHECK_THAT(review.messages[1].content, ContainsSubstring("Applied: player.location = \"lighthouse\""));
    CHECK_THAT(review.messages[1].content, ContainsSubstring("# Narration\nYou walk to the lighthouse.\n"));
    CHECK(calls[2].spec.grammar == turn::BuildReviewGrammar(true, true));
    CHECK(calls[2].spec.maxTokens == 384);
    CHECK(calls[2].spec.thinkingMaxTokens == 0);
    CHECK(calls[2].spec.sampling.seed == core::SamplingSeed(42, 1, core::CallKind::Review, 0));

    const nlohmann::json& reviews = record->timings.at("review");
    REQUIRE(reviews.size() == 1);
    CHECK(reviews[0].at("consistent") == true);
    CHECK(reviews[0].at("digest") == "The player walked to the lighthouse.");
    CHECK(reviews[0].contains("prompt_tokens"));
    CHECK_FALSE(reviews[0].contains("problems"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("A narration that contradicts the outcome is narrated once more and replaced", "[session][review]")
{
    const std::string contradicts =
        R"({"consistent":false,"problems":["The climb failed, but the narration has the player reach the top."],)"
        R"("digest":"The player reached the top."})";
    const std::string fixed = R"({"consistent":true,"problems":[],"digest":"The player slipped on the cliff."})";
    ReviewScript script{CLIMB, {"You reach the top with ease.", "You slip on the wet rock."}, {contradicts, fixed}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_renarrate"), 42, backend);
    std::vector<std::vector<std::string>> restarts;
    std::string streamed;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>& problems) { restarts.push_back(problems); };
    callbacks.onNarrationToken = [&](std::string_view piece) { streamed += piece; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I climb the cliff.", callbacks, {});
    REQUIRE(record);

    // The second narration and the second review's digest are committed.
    CHECK(record->narration == "You slip on the wet rock.");
    CHECK(record->digest == "The player slipped on the cliff.");
    REQUIRE(restarts.size() == 1);
    CHECK(restarts[0] == std::vector<std::string>{"The climb failed, but the narration has the player reach the top."});
    CHECK(streamed == "You reach the top with ease.You slip on the wet rock."); // Both stream; the UI clears on the restart.

    // The renarrate is the narrate prompt with the correction before the instruction, with its own seed.
    const auto& calls = backend.Calls(); // resolve, narrate, review, renarrate, review
    REQUIRE(calls.size() == 5);
    const inference::Prompt& first = calls[1].prompt;
    const inference::Prompt& again = calls[3].prompt;
    REQUIRE(again.messages.size() == first.messages.size());
    for (std::size_t i = 0; i + 1 < first.messages.size(); ++i) {
        CHECK(again.messages[i].content == first.messages[i].content);
    }
    CHECK_THAT(first.messages.back().content, !ContainsSubstring(std::string(turn::RENARRATE_CORRECTION)));
    CHECK_THAT(again.messages.back().content,
               ContainsSubstring(std::string(turn::RENARRATE_CORRECTION) +
                                 "\n- The climb failed, but the narration has the player reach the top.\n\n" +
                                 std::string(turn::NARRATE_INSTRUCTION)));
    CHECK(calls[3].spec.grammar.empty());
    CHECK(calls[3].spec.sampling.seed == core::SamplingSeed(42, 1, core::CallKind::Renarrate, 0));
    // The second review reads the second narration, seeded apart from any first review.
    CHECK_THAT(calls[4].prompt.messages[1].content, ContainsSubstring("# Narration\nYou slip on the wet rock.\n"));
    CHECK(calls[4].spec.sampling.seed ==
          core::SamplingSeed(42, 1, core::CallKind::Review, turn::REVIEW_RETRY_OFFSET));

    // Both verdicts, the renarrate's call and the replaced text are in the telemetry.
    const nlohmann::json& reviews = record->timings.at("review");
    REQUIRE(reviews.size() == 2);
    CHECK(reviews[0].at("consistent") == false);
    CHECK(reviews[0].at("problems") ==
          nlohmann::json::array({"The climb failed, but the narration has the player reach the top."}));
    CHECK(reviews[1].at("consistent") == true);
    CHECK(record->timings.at("renarrate").at("replaced") == "You reach the top with ease.");
    CHECK(record->timings.at("renarrate").contains("generated"));
    CHECK(record->timings.at("narrate").contains("generated")); // The first narration's call stays.
}

TEST_CASE("A renarrated turn is committed whatever its second review says", "[session][review]")
{
    const std::string first = R"({"consistent":false,"problems":["The player flies."],"digest":"The player flew."})";
    const std::string second =
        R"({"consistent":false,"problems":["The player still flies."],"digest":"The player flew again."})";
    ReviewScript script{MOVE, {"You fly to the lighthouse.", "You float to the lighthouse."}, {first, second}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_renarrate_twice"), 42, backend);
    int restarts = 0;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { ++restarts; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->narration == "You float to the lighthouse.");
    CHECK(record->digest == "The player flew again.");
    CHECK(restarts == 1);
    CHECK(backend.Calls().size() == 5); // No second renarrate.
    CHECK(record->timings.at("review").at(1).at("consistent") == false);
}

TEST_CASE("An unreadable review commits the narration with no digest and says why", "[session][review]")
{
    ReviewScript script{MOVE, {"You walk to the lighthouse."}, {R"({"consistent":tr)"}}; // Cut off.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_review_unreadable"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(record->digest.empty());
    CHECK(backend.Calls().size() == 3); // As if the review had found nothing: no renarrate.
    const nlohmann::json& review = record->timings.at("review").at(0);
    CHECK_THAT(review.at("error").get<std::string>(), ContainsSubstring("not complete JSON"));
    CHECK_FALSE(review.contains("consistent"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("With only the compact transcript on, the review writes the digest and never renarrates", "[session][review]")
{
    // The output says the narration contradicts the outcome, but the check is off: only the digest is read.
    ReviewScript script{MOVE, {"You fly to the lighthouse."},
                        {R"({"consistent":false,"problems":["The player flies."],"digest":"The player went to the lighthouse."})"}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, true),
                                                 NewRunFile("session_review_digest_only"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    const auto& calls = backend.Calls(); // resolve, narrate, review
    REQUIRE(calls.size() == 3);
    CHECK(calls[2].spec.grammar == turn::BuildReviewGrammar(false, true));
    CHECK(record->narration == "You fly to the lighthouse.");
    CHECK(record->digest == "The player went to the lighthouse.");
    CHECK_FALSE(record->timings.at("review").at(0).contains("consistent"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("With both switches off no review call is made", "[session][review]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk to the lighthouse."));
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, false),
                                                 NewRunFile("session_review_off"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    CHECK(backend.Calls().size() == 2); // resolve, narrate
    CHECK_FALSE(record->timings.contains("review"));
    CHECK(record->digest.empty());
}

TEST_CASE("A cancel during the review or the renarrate commits nothing", "[session][review]")
{
    const std::string contradicts = R"({"consistent":false,"problems":["The player flies."],"digest":"The player flew."})";
    ReviewScript script{MOVE, {"You fly to the lighthouse.", "You walk to the lighthouse."}, {contradicts, CONSISTENT}};
    const test::ScriptedBackend::Responder scripted = script.Responder();
    std::stop_source stop;
    turn::TurnCallbacks callbacks;

    SECTION("during the review")
    {
        // The stop arrives as the review call starts, so its reply never streams.
        test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
            if (prompt.messages.front().content == turn::REVIEW_RULES) {
                stop.request_stop();
            }
            return scripted(prompt, spec);
        });
        turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                     NewRunFile("session_review_cancel"), 42, backend);
        CHECK_FALSE(session.RunTurn("I walk to the lighthouse.", callbacks, stop.get_token()));
        CHECK(session.History().empty());
        CHECK(session.State() == MiniWorld().initialState);
    }
    SECTION("during the renarrate")
    {
        test::ScriptedBackend backend(16384, scripted);
        turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                     NewRunFile("session_renarrate_cancel"), 42, backend);
        callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { stop.request_stop(); };
        CHECK_FALSE(session.RunTurn("I walk to the lighthouse.", callbacks, stop.get_token()));
        CHECK(backend.Calls().size() == 4); // resolve, narrate, review, and the cancelled renarrate.
        CHECK(session.History().empty());
        CHECK(session.State() == MiniWorld().initialState);
    }
}

TEST_CASE("With compact_transcript on, older turns reach the prompt as their digest", "[session][review]")
{
    const fs::path file = NewRunFile("session_compact");
    {
        core::Settings settings = ReviewSettings(false, true);
        settings.proseTurns = 1;
        ReviewScript script{MOVE,
                            {"Narration one.", "Narration two.", "Narration three."},
                            {R"({"digest":"Digest one."})", R"({"digest":"Digest two."})", R"({"digest":"Digest three."})"}};
        test::ScriptedBackend backend(16384, script.Responder());
        turn::Session session = turn::Session::Start(MiniWorld(), settings, file, 42, backend);
        for (const char* input : {"I walk.", "I walk on.", "I walk further."}) {
            REQUIRE(session.RunTurn(input, {}, {}));
        }

        // The third turn's resolve prompt: turn 1 as its digest, turn 2 (the newest) as its narration.
        const auto& calls = backend.Calls(); // Per turn: resolve, narrate, review.
        REQUIRE(calls.size() == 9);
        const inference::Prompt& third = calls[6].prompt;
        REQUIRE(third.messages.size() == 1 + 4 + 1);
        CHECK(third.messages[1].content.starts_with("I walk.")); // The input stays.
        CHECK(third.messages[2].content == "Digest one.");
        CHECK(third.messages[4].content == "Narration two.");
        CHECK_THAT(third.messages[0].content, ContainsSubstring("Older turns in the transcript show a short summary"));
        // The history keeps the full narration (the Play window shows it).
        CHECK(session.History().at(0).narration == "Narration one.");
    }

    // With compact_transcript off, the same run renders every turn in full, its stored digests notwithstanding.
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Load(MiniWorld(), TestSettings(), file, backend);
    REQUIRE(session.RunTurn("I stop.", {}, {}));
    const inference::Prompt& fourth = backend.Calls().at(0).prompt;
    REQUIRE(fourth.messages.size() == 1 + 6 + 1);
    CHECK(fourth.messages[2].content == "Narration one.");
    CHECK(fourth.messages[4].content == "Narration two.");
}

TEST_CASE("Retrieval searches with the scene: moving the player changes the lore in the prompt", "[session][scene]")
{
    const std::string climb =
        R"({"intent":"climb","checks":[],"changes":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs"}]})";
    test::ScriptedBackend backend(16384, Responder(climb, "You look around."));
    turn::Session session = turn::Session::Start(core::LoadWorld(test::SceneWorldDir("session_scene_world")),
                                                 TestSettings(), NewRunFile("session_scene"), 42, backend);
    // A lore hit's reasons in a turn's telemetry; null when it did not reach the prompt.
    auto reasonsOf = [](const core::TurnRecord& record, const std::string& subject) {
        for (const nlohmann::json& hit : record.timings.at("retrieval")) {
            if (hit.at("source") == "lore" && hit.at("ref") == subject) {
                return hit.at("reasons");
            }
        }
        return nlohmann::json();
    };

    // On the shore the input names nothing, but the scene names the shore.
    const std::optional<core::TurnRecord> first = session.RunTurn("I look around.", {}, {});
    REQUIRE(first);
    CHECK(first->timings.at("scene") == nlohmann::json::array({"shore"}));
    CHECK(reasonsOf(*first, "shore") == nlohmann::json::array({"scene"}));
    CHECK_THAT(Tail(backend.Calls().at(0).prompt), ContainsSubstring("- shore: Grey pebbles"));

    // The first turn moved the player to the cliff top: the same input now brings the cliff top's lore.
    const std::optional<core::TurnRecord> second = session.RunTurn("I look around.", {}, {});
    REQUIRE(second);
    CHECK(second->timings.at("scene") == nlohmann::json::array({"cliff top"}));
    CHECK(reasonsOf(*second, "cliff top") == nlohmann::json::array({"scene"}));
    const nlohmann::json shore = reasonsOf(*second, "shore");
    CHECK((shore.is_null() || shore.empty())); // No longer named by the scene.
    CHECK_THAT(Tail(backend.Calls().at(2).prompt), ContainsSubstring("- cliff top: Wind howls"));
}

TEST_CASE("Retrieval searches with the previous turn's digest, without a boost", "[session][scene]")
{
    ReviewScript script{MOVE, {"You wait.", "You wait some more."},
                        {R"({"digest":"Storms arrive at dusk."})", R"({"digest":"The player waited."})"}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, true),
                                                 NewRunFile("session_digest_query"), 42, backend);
    REQUIRE(session.RunTurn("I wait.", {}, {}));
    const std::optional<core::TurnRecord> second = session.RunTurn("I wait.", {}, {});
    REQUIRE(second);

    // The second query is the input, then the first turn's digest (the mini world declares no scene). The digest
    // names the storm, but only the input and the scene boost.
    const std::vector<std::vector<float>> vectors =
        backend.Embed({"I wait.\nStorms arrive at dusk.", "storm: Storms arrive at dusk."}, inference::EmbedPurpose::Query);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    const nlohmann::json* pStorm = nullptr;
    for (const nlohmann::json& hit : second->timings.at("retrieval")) {
        if (hit.at("ref") == "storm") {
            pStorm = &hit;
        }
    }
    REQUIRE(pStorm != nullptr);
    CHECK(pStorm->at("score").get<double>() == Catch::Approx(similarity));
    CHECK(pStorm->at("reasons").empty());
    CHECK(second->timings.at("scene") == nlohmann::json::array());
}
