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

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
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

TEST_CASE("Engine mode makes no lookup call and keeps no notes", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(R"({"intent":"listen","checks":[],"changes":[]})"));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_engine_mode"), 42, backend);
    REQUIRE(session.RunTurn("I listen.", {}, {}));
    CHECK(backend.Calls().size() == 2);
    CHECK_THAT(backend.Calls().at(0).prompt.messages.front().content, !ContainsSubstring("# Memory"));
}
