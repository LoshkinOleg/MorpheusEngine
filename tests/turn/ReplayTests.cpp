#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "turn/Replay.h"
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
    for (const fs::directory_entry& entry : fs::directory_iterator(test::TestDataDir() / "replays")) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        DYNAMIC_SECTION(entry.path().filename().string())
        {
            const nlohmann::json fixture = ReadJson(entry.path());
            std::map<std::string, std::deque<std::string>> replies;
            for (const auto& [kind, list] : fixture.at("replies").items()) {
                for (const nlohmann::json& reply : list) {
                    replies[kind].push_back(reply.is_string() ? reply.get<std::string>() : reply.dump());
                }
            }
            test::ScriptedBackend backend(16384, [&replies](const inference::Prompt& prompt,
                                                            const inference::GenerationSpec& spec) {
                const core::CallKind kind = turn::CallKindOf(prompt, spec);
                std::deque<std::string>& queue = replies[turn::CallKindName(kind)];
                if (queue.empty()) {
                    // Fold timing depends on the tokenizer, so a replay may fold where the recording did not.
                    MORPHEUS_REQUIRE(kind == core::CallKind::Summarize,
                                     "replay ran out of " + turn::CallKindName(kind) + " replies");
                    return std::string("Summary.");
                }
                std::string reply = std::move(queue.front());
                queue.pop_front();
                return reply;
            });

            core::Settings settings = ReplaySettings();
            settings.memoryMode = core::ParseMemoryMode(fixture.at("memory_mode").get<std::string>());
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
                } else {
                    REQUIRE(session.Regenerate({}, {}));
                    REQUIRE(session.Maintain({}));
                }
            }

            const nlohmann::json& expected = fixture.at("expected");
            const std::vector<core::TurnRecord> history = session.History();
            REQUIRE(history.size() == expected.at("turns").size());
            for (std::size_t i = 0; i < history.size(); ++i) {
                CHECK(turn::TurnResultJson(history[i]) == expected.at("turns")[i]);
            }
            CHECK(session.State() == expected.at("final_state"));
        }
    }
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
