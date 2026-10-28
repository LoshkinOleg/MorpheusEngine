#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "TestPaths.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/Worker.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

const std::string MOVE = R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks there"}]})";

core::Settings TestSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf";
    settings.embeddingModel = "models/embed.gguf";
    settings.contextSize = 16384;
    settings.budget = {256, 256, 256, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

test::ScriptedBackend::Responder Responder(std::string narration)
{
    return [narration](const inference::Prompt&, const inference::GenerationSpec& spec) {
        return spec.grammar.find("intent") != std::string::npos ? MOVE : narration;
    };
}

turn::StartRun Start(const std::string& name)
{
    return {test::TestDataDir() / "worlds" / "mini", test::FreshTempDir(name) / "run.db", 7};
}

// The events of one command, through its closing event. Fails the test if any wait exceeds 10 s.
std::vector<turn::Event> UntilClosing(turn::Worker& worker)
{
    std::vector<turn::Event> events;
    for (;;) {
        std::optional<turn::Event> event = worker.WaitEvent(std::chrono::seconds(10));
        REQUIRE(event);
        events.push_back(std::move(*event));
        if (turn::IsClosing(events.back())) {
            return events;
        }
    }
}

} // namespace

TEST_CASE("The worker opens a run and plays a turn, streaming events", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder("You walk to the lighthouse."));
    turn::Worker worker(TestSettings(), backend);

    worker.Post(Start("worker_turn"));
    const std::vector<turn::Event> opened = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::RunOpened>(opened.back()));
    CHECK(std::get<turn::RunOpened>(opened.back()).history.empty());

    worker.Post(turn::SubmitInput{"I walk to the lighthouse."});
    const std::vector<turn::Event> events = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::TurnResolved>(events.front()));
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(events.back()));
    std::string streamed;
    for (const turn::Event& event : events) {
        if (const auto* pPiece = std::get_if<turn::TokenStreamed>(&event)) {
            streamed += pPiece->piece;
        }
    }
    const turn::TurnCompleted& completed = std::get<turn::TurnCompleted>(events.back());
    CHECK(streamed == "You walk to the lighthouse.");
    CHECK(completed.record.number == 1);
    CHECK(completed.state.at("player").at("fields").at("location") == "lighthouse");
    CHECK_FALSE(completed.assembly.prompt.messages.empty());

    worker.Post(turn::UndoTurn{});
    const std::vector<turn::Event> undone = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::StateChanged>(undone.back()));
    CHECK(std::get<turn::StateChanged>(undone.back()).history.empty());
}

TEST_CASE("Commands without an open run end with an Error event", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder("x"));
    turn::Worker worker(TestSettings(), backend);
    worker.Post(turn::SubmitInput{"I walk."});
    const std::vector<turn::Event> events = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::Error>(events.back()));
    CHECK_THAT(std::get<turn::Error>(events.back()).message, ContainsSubstring("no run is open"));
}

TEST_CASE("Cancel stops a running turn and commits nothing", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder(std::string(400, 'w'))); // 100 pieces
    backend.SetPieceDelay(std::chrono::milliseconds(20));
    turn::Worker worker(TestSettings(), backend);
    worker.Post(Start("worker_cancel"));
    UntilClosing(worker);

    worker.Post(turn::SubmitInput{"I walk."});
    for (;;) {
        std::optional<turn::Event> event = worker.WaitEvent(std::chrono::seconds(10));
        REQUIRE(event);
        if (std::holds_alternative<turn::TokenStreamed>(*event)) {
            break;
        }
    }
    worker.Cancel();
    const std::vector<turn::Event> rest = UntilClosing(worker);
    CHECK(std::holds_alternative<turn::TurnCancelled>(rest.back()));

    worker.Post(turn::UndoTurn{});
    const std::vector<turn::Event> undo = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::Error>(undo.back())); // Nothing was committed.
}

TEST_CASE("Destroying a busy worker cancels its job instead of waiting for it", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder(std::string(400, 'w'))); // 100 pieces x 20 ms = 2 s
    backend.SetPieceDelay(std::chrono::milliseconds(20));
    const auto started = std::chrono::steady_clock::now();
    {
        turn::Worker worker(TestSettings(), backend);
        worker.Post(Start("worker_shutdown"));
        UntilClosing(worker);
        worker.Post(turn::SubmitInput{"I walk."});
        while (true) {
            std::optional<turn::Event> event = worker.WaitEvent(std::chrono::seconds(10));
            REQUIRE(event);
            if (std::holds_alternative<turn::TokenStreamed>(*event)) {
                break;
            }
        }
    }
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500));
}
