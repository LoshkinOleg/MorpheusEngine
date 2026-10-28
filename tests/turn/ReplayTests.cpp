#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/Replay.h"
#include "turn/ResolveGrammar.h"
#include "turn/Session.h"

using namespace morpheus;
namespace fs = std::filesystem;

namespace {

core::Settings ReplaySettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf";
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {512, 768, 512, 2048, 1024, 4096, 2048, 768}; // As settings.example.json.
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

nlohmann::json ReadJson(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    return nlohmann::json::parse(stream);
}

} // namespace

TEST_CASE("Recorded sessions replay to the same turns and state", "[replay]")
{
    int fixturesRun = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(test::TestDataDir() / "replays")) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        ++fixturesRun;
        DYNAMIC_SECTION(entry.path().filename().string())
        {
            const std::string fixtureName = entry.path().filename().string();
            INFO("fixture " << fixtureName);
            const nlohmann::json fixture = ReadJson(entry.path());
            std::map<std::string, std::deque<std::string>> replies;
            for (const auto& [kind, list] : fixture.at("replies").items()) {
                for (const nlohmann::json& reply : list) {
                    replies[kind].push_back(reply.is_string() ? reply.get<std::string>() : reply.dump());
                }
            }
            int summaryFallbacks = 0; // Summaries the replay made up because the recording had none left.
            test::ScriptedBackend backend(16384, [&replies, &summaryFallbacks](const inference::Prompt& prompt,
                                                                               const inference::GenerationSpec& spec) {
                const core::CallKind kind = turn::CallKindOf(prompt, spec);
                std::deque<std::string>& queue = replies[turn::CallKindName(kind)];
                if (queue.empty()) {
                    // Fold timing depends on the tokenizer, so a replay may fold where the recording did not.
                    MORPHEUS_REQUIRE(kind == core::CallKind::Summarize,
                                     "replay ran out of " + turn::CallKindName(kind) + " replies");
                    ++summaryFallbacks;
                    return std::string("Summary.");
                }
                std::string reply = std::move(queue.front());
                queue.pop_front();
                return reply;
            });

            // A misspelt memory mode must fail the replay, not fall back to the world's default.
            const std::string modeText = fixture.at("memory_mode").get<std::string>();
            const std::optional<core::MemoryMode> mode = core::ParseMemoryMode(modeText);
            INFO("memory_mode '" << modeText << "'");
            REQUIRE(mode.has_value());
            core::Settings settings = ReplaySettings();
            settings.memoryMode = *mode;
            turn::Session session = turn::Session::Start(
                core::LoadWorld(test::RepoDir() / fixture.at("world").get<std::string>()), settings,
                test::FreshTempDir("replay_" + entry.path().stem().string()) / "run.db",
                fixture.at("seed").get<std::uint64_t>(), backend);

            for (const nlohmann::json& step : fixture.at("steps")) {
                if (step.contains("input")) {
                    REQUIRE(session.RunTurn(step.at("input").get<std::string>(), {}, {}));
                    REQUIRE(session.Maintain({}));
                } else if (step.contains("undo")) {
                    session.Undo();
                } else if (step.contains("regenerate")) {
                    REQUIRE(session.Regenerate({}, {}));
                    REQUIRE(session.Maintain({}));
                } else {
                    FAIL("fixture " << fixtureName << " has an unknown step: " << step.dump());
                }
            }

            const nlohmann::json& expected = fixture.at("expected");
            const std::vector<core::TurnRecord> history = session.History();
            REQUIRE(history.size() == expected.at("turns").size());
            for (std::size_t i = 0; i < history.size(); ++i) {
                CHECK(turn::TurnResultJson(history[i]) == expected.at("turns")[i]);
            }
            CHECK(session.State() == expected.at("final_state"));

            // A recorded reply the engine never asked for means the replay diverged from the recording. Summaries
            // are exempt: where they are made depends on the tokenizer.
            for (const auto& [kind, queue] : replies) {
                if (kind != "summarize") {
                    INFO("unused '" << kind << "' replies");
                    CHECK(queue.empty());
                }
            }
            // Make a replay that folds differently from its recording visible.
            if (summaryFallbacks > 0) {
                WARN(fixtureName << ": " << summaryFallbacks << " summary fallback(s)");
            }
        }
    }
    REQUIRE(fixturesRun > 0); // An empty or missing fixture directory must not pass silently.
}

TEST_CASE("CallKindOf tells the four calls apart and CallKindName spells them", "[replay]")
{
    const core::World world = core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
    const inference::Prompt narration{{{inference::Role::System, "story"}, {inference::Role::User, "I walk."}}, 1};
    const inference::Prompt summarize{{{inference::Role::System, "story"},
                                       {inference::Role::User, std::string(turn::SUMMARIZE_INSTRUCTION)}},
                                      1};
    const inference::Prompt condense{{{inference::Role::System, "story"},
                                      {inference::Role::User, std::string(turn::CONDENSE_INSTRUCTION)}},
                                     1};
    inference::GenerationSpec lookupSpec;
    lookupSpec.grammar = turn::BuildLookupGrammar();
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);
    const inference::GenerationSpec plainSpec;

    CHECK(turn::CallKindOf(narration, lookupSpec) == core::CallKind::Lookup);
    CHECK(turn::CallKindOf(narration, resolveSpec) == core::CallKind::Resolve);
    CHECK(turn::CallKindOf(summarize, plainSpec) == core::CallKind::Summarize);
    CHECK(turn::CallKindOf(condense, plainSpec) == core::CallKind::Summarize);
    CHECK(turn::CallKindOf(narration, plainSpec) == core::CallKind::Narrate);
    CHECK(turn::CallKindOf(inference::Prompt{}, plainSpec) == core::CallKind::Narrate);

    CHECK(turn::CallKindName(core::CallKind::Lookup) == "lookup");
    CHECK(turn::CallKindName(core::CallKind::Resolve) == "resolve");
    CHECK(turn::CallKindName(core::CallKind::Narrate) == "narrate");
    CHECK(turn::CallKindName(core::CallKind::Summarize) == "summarize");
}

TEST_CASE("A resolve grammar is not taken for a lookup when the world has a field named lookups", "[replay]")
{
    // An edited copy of the mini world whose character has a "lookups" string field.
    const fs::path folder = test::FreshTempDir("replay_lookups_world") / "mini"; // The folder name must be the id.
    fs::create_directories(folder);
    for (const char* file : {"world.json", "rules.md", "lore.csv"}) {
        fs::copy_file(test::TestDataDir() / "worlds" / "mini" / file, folder / file);
    }
    nlohmann::json schema = ReadJson(test::TestDataDir() / "worlds" / "mini" / "state_schema.json");
    schema["entity_types"]["character"]["fields"]["lookups"] = {{"type", "string"}};
    test::WriteFile(folder / "state_schema.json", schema.dump());
    nlohmann::json initial = ReadJson(test::TestDataDir() / "worlds" / "mini" / "initial_state.json");
    initial["player"]["fields"]["lookups"] = "none";
    test::WriteFile(folder / "initial_state.json", initial.dump());

    const core::World world = core::LoadWorld(folder);
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);
    REQUIRE(resolveSpec.grammar.find("lookups") != std::string::npos); // The case is real.
    CHECK(turn::CallKindOf(inference::Prompt{}, resolveSpec) == core::CallKind::Resolve);

    inference::GenerationSpec lookupSpec;
    lookupSpec.grammar = turn::BuildLookupGrammar();
    CHECK(turn::CallKindOf(inference::Prompt{}, lookupSpec) == core::CallKind::Lookup);
}

TEST_CASE("RecordingBackend records each call's text by kind and MakeReplayFixture writes the format", "[replay]")
{
    test::ScriptedBackend inner(16384, [](const inference::Prompt&, const inference::GenerationSpec& spec) {
        return spec.grammar.empty() ? std::string("You walk.") : std::string(R"({"intent":"walk","checks":[],"changes":[]})");
    });
    turn::RecordingBackend recording(inner);
    turn::Session session = turn::Session::Start(core::LoadWorld(test::TestDataDir() / "worlds" / "mini"),
                                                 ReplaySettings(), test::FreshTempDir("replay_record") / "run.db", 9,
                                                 recording);
    REQUIRE(session.RunTurn("I walk.", {}, {}));

    CHECK(recording.Replies().at(core::CallKind::Resolve) == std::vector<std::string>{R"({"intent":"walk","checks":[],"changes":[]})"});
    CHECK(recording.Replies().at(core::CallKind::Narrate) == std::vector<std::string>{"You walk."});

    const nlohmann::json fixture = turn::MakeReplayFixture("tests/data/worlds/mini", 9, core::MemoryMode::Engine,
                                                           {{turn::ReplayStep::Action::Input, "I walk."}}, recording,
                                                           session.History(), session.State());
    CHECK(fixture.at("world") == "tests/data/worlds/mini");
    CHECK(fixture.at("seed") == 9);
    CHECK(fixture.at("memory_mode") == "engine");
    CHECK(fixture.at("steps") == nlohmann::json::parse(R"([{"input": "I walk."}])"));
    CHECK(fixture.at("replies").at("narrate") == nlohmann::json::array({"You walk."}));
    CHECK(fixture.at("expected").at("turns").at(0).at("narration") == "You walk.");
    CHECK(fixture.at("expected").at("final_state") == session.State());
}
