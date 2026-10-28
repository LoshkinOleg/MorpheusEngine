#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
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
    core::Settings settings = TestSettings();
    // FakeBackend counts 1 token per 4 bytes: the system message is about 700 tokens, so L3 gets about 1000, while
    // each turn adds about 520 (a 2000-byte narration). Folding must start by the third turn.
    settings.budget = {64, 600, 200, 400, 1024, 1024, 512, 100};
    test::ScriptedBackend backend(3000, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_folding"), 42, backend);

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
