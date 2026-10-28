#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "TestPaths.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "turn/Worker.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

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
    test::ScriptedBackend backend(16384, Responder(std::string(400, 'w'))); // 100 pieces x 20 ms = 2 s at least
    backend.SetPieceDelay(std::chrono::milliseconds(20));
    std::chrono::steady_clock::time_point streaming = {};
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
        streaming = std::chrono::steady_clock::now(); // Narration has begun; the worker is destroyed next.
    }
    CHECK(std::chrono::steady_clock::now() - streaming < std::chrono::milliseconds(500));
}

TEST_CASE("A turn posted before the previous one commits still sees its notes", "[worker]")
{
    core::Settings settings = TestSettings();
    settings.memoryMode = core::MemoryMode::Assisted;
    const std::string listen =
        R"({"intent":"listen","checks":[],"changes":[],"notes":["The keeper hums an old sea shanty."]})";
    test::ScriptedBackend backend(16384, [&](const inference::Prompt&, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("lookups") != std::string::npos) {
            return std::string(R"({"lookups":[{"source":"lore","query":"keeper"}]})");
        }
        return spec.grammar.find("intent") != std::string::npos ? listen : std::string("The keeper hums.");
    });
    backend.SetPieceDelay(std::chrono::milliseconds(1)); // The first turn is still running when the second is posted.
    turn::Worker worker(settings, backend);
    worker.Post(Start("worker_notes"));
    REQUIRE(std::holds_alternative<turn::RunOpened>(UntilClosing(worker).back()));

    // The second turn is queued before the first commits, so it is already waiting when maintenance falls due.
    worker.Post(turn::SubmitInput{"I listen."});
    worker.Post(turn::SubmitInput{"What shanty does the keeper hum?"});
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(UntilClosing(worker).back()));
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(UntilClosing(worker).back()));

    // The second turn's resolve prompt is the last call whose grammar asks for an intent.
    const test::ScriptedBackend::Call* pResolve = nullptr;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        if (call.spec.grammar.find("intent") != std::string::npos &&
            call.spec.grammar.find("lookups") == std::string::npos) {
            pResolve = &call;
        }
    }
    REQUIRE(pResolve != nullptr);
    CHECK_THAT(pResolve->prompt.messages.back().content,
               ContainsSubstring("Note from turn 1: The keeper hums an old sea shanty."));
}

TEST_CASE("A command cancelled before the worker picks it up closes with TurnCancelled", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder(std::string(400, 'w'))); // 100 pieces
    backend.SetPieceDelay(std::chrono::milliseconds(20));
    turn::Worker worker(TestSettings(), backend);
    worker.Post(Start("worker_early_cancel"));
    UntilClosing(worker);

    worker.Post(turn::SubmitInput{"I walk."});
    for (;;) {
        std::optional<turn::Event> event = worker.WaitEvent(std::chrono::seconds(10));
        REQUIRE(event);
        if (std::holds_alternative<turn::TokenStreamed>(*event)) {
            break; // The worker is busy narrating.
        }
    }
    worker.Post(turn::SubmitInput{"I walk again."});
    worker.Cancel();
    CHECK(std::holds_alternative<turn::TurnCancelled>(UntilClosing(worker).back())); // The running turn.
    const std::vector<turn::Event> queued = UntilClosing(worker);
    CHECK(queued.size() == 1); // Closed without running: nothing resolved or streamed.
    CHECK(std::holds_alternative<turn::TurnCancelled>(queued.back()));

    worker.Post(turn::UndoTurn{});
    const std::vector<turn::Event> undo = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::Error>(undo.back())); // Nothing was committed.
}

TEST_CASE("RegenerateTurn plays the latest turn again as a new attempt", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder("You walk to the lighthouse."));
    turn::Worker worker(TestSettings(), backend);
    worker.Post(Start("worker_regenerate"));
    UntilClosing(worker);
    worker.Post(turn::SubmitInput{"I walk to the lighthouse."});
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(UntilClosing(worker).back()));

    worker.Post(turn::RegenerateTurn{});
    const std::vector<turn::Event> events = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(events.back()));
    const turn::TurnCompleted& completed = std::get<turn::TurnCompleted>(events.back());
    CHECK(completed.record.number == 1);
    CHECK(completed.record.attempt == 1);
}

TEST_CASE("LoadRun reopens a closed run with its history", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder("You walk to the lighthouse."));
    const turn::StartRun start = Start("worker_load");
    {
        turn::Worker worker(TestSettings(), backend);
        worker.Post(start);
        UntilClosing(worker);
        worker.Post(turn::SubmitInput{"I walk to the lighthouse."});
        REQUIRE(std::holds_alternative<turn::TurnCompleted>(UntilClosing(worker).back()));
    } // Destroying the worker closes the run.

    turn::Worker worker(TestSettings(), backend);
    worker.Post(turn::LoadRun{start.worldFolder, start.runFile});
    const std::vector<turn::Event> events = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::RunOpened>(events.back()));
    const turn::RunOpened& opened = std::get<turn::RunOpened>(events.back());
    REQUIRE(opened.history.size() == 1);
    CHECK(opened.history.front().number == 1);
    CHECK(opened.state.at("player").at("fields").at("location") == "lighthouse");
    CHECK_FALSE(opened.worldChanged);
}

TEST_CASE("PollEvents returns the queued events in order without blocking", "[worker]")
{
    test::ScriptedBackend backend(16384, Responder("You walk to the lighthouse."));
    turn::Worker worker(TestSettings(), backend);
    const auto polled = std::chrono::steady_clock::now();
    CHECK(worker.PollEvents().empty()); // Nothing posted: returns at once instead of waiting.
    CHECK(std::chrono::steady_clock::now() - polled < std::chrono::milliseconds(100));

    worker.Post(Start("worker_poll"));
    worker.Post(turn::SubmitInput{"I walk to the lighthouse."});
    std::vector<turn::Event> events;
    int closing = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (closing < 2) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        for (turn::Event& event : worker.PollEvents()) {
            closing += turn::IsClosing(event) ? 1 : 0;
            events.push_back(std::move(event));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(worker.PollEvents().empty()); // Polling drained the queue.

    // Both commands' events, in the order they were emitted.
    REQUIRE(events.size() >= 4);
    CHECK(std::holds_alternative<turn::RunOpened>(events[0]));
    CHECK(std::holds_alternative<turn::TurnResolved>(events[1]));
    std::string streamed;
    for (std::size_t i = 2; i + 1 < events.size(); ++i) {
        REQUIRE(std::holds_alternative<turn::TokenStreamed>(events[i]));
        streamed += std::get<turn::TokenStreamed>(events[i]).piece;
    }
    CHECK(streamed == "You walk to the lighthouse.");
    CHECK(std::holds_alternative<turn::TurnCompleted>(events.back()));
}
