// morpheus_play: plays a world in the terminal through the engine worker, the same path the app will take.
//
// Usage: morpheus_play <settings.json> <world folder> <run.db> [--seed N] [--record <fixture.json>]
// An existing run file is continued; otherwise a new run starts (seeded by --seed, else randomly). --record needs a
// new run and writes a replay fixture (see tests/data/replays) when you quit. Run it from the repo root with a
// repo-relative world path: the fixture stores that path as given. Failed steps are rolled back out of the recording,
// except summary replies (see RecordingBackend::RollBack).
// A maintenance failure reported before you quit refuses the recording; maintenance still running at /quit is
// cancelled by shutdown and not recorded.
// At the prompt: /undo, /regen, /state, /quit, or your action.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "core/Text.h"
#include "core/World.h"
#include "llama/LlamaCppBackend.h"
#include "turn/Replay.h"
#include "turn/Worker.h"

using namespace morpheus;

namespace {

constexpr std::string_view USAGE =
    "usage: morpheus_play <settings.json> <world folder> <run.db> [--seed N] [--record <fixture.json>]\n";

// What the player has seen of the run, kept from the events for /state and the replay fixture.
struct RunView {
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
    bool maintenanceFailed = false; // Set when any maintenance pass failed (checked at quit when recording).
};

// How a command closed.
enum class StepOutcome { Completed, Cancelled, Failed };

// Prints one command's events up to its closing event and updates `view`. Returns how the command closed.
StepOutcome PrintUntilClosing(turn::Worker& worker, RunView& view)
{
    for (;;) {
        const std::optional<turn::Event> event = worker.WaitEvent(std::chrono::milliseconds(100));
        if (!event) {
            continue;
        }
        if (const auto* pPiece = std::get_if<turn::TokenStreamed>(&*event)) {
            std::cout << pPiece->piece << std::flush;
        } else if (const auto* pResolved = std::get_if<turn::TurnResolved>(&*event)) {
            const std::string mechanics = context::RenderMechanicsLine(pResolved->record);
            if (!mechanics.empty()) {
                std::cout << mechanics << '\n';
            }
            for (const core::RejectedChange& rejected : pResolved->record.rejected) {
                std::cout << "  (rejected: " << rejected.reason << ")\n";
            }
            std::cout << '\n';
        } else if (const auto* pFailed = std::get_if<turn::MaintenanceFailed>(&*event)) {
            std::cerr << "maintenance: " << pFailed->message << '\n';
            view.maintenanceFailed = true;
        } else if (const auto* pCompleted = std::get_if<turn::TurnCompleted>(&*event)) {
            // "[resolve 41 tok in 830 ms (49.4 tok/s), narrate 120 tok in 2100 ms; prompt 2311 tok, 2290 reused]"
            const nlohmann::json& resolve = pCompleted->record.timings.at("resolve");
            const nlohmann::json& narrate = pCompleted->record.timings.at("narrate");
            const int resolveTokens = resolve.at("generated").get<int>();
            const double resolveMs = resolve.at("generate_ms").get<double>();
            std::cout << std::format("\n[resolve {} tok in {:.0f} ms ({:.1f} tok/s), narrate {} tok in {:.0f} ms; "
                                     "prompt {} tok, {} reused]\n",
                                     resolveTokens, resolveMs, resolveTokens * 1000.0 / std::max(1.0, resolveMs),
                                     narrate.at("generated").get<int>(), narrate.at("generate_ms").get<double>(),
                                     resolve.at("prompt_tokens").get<int>(), resolve.at("reused").get<int>());
            if (!view.history.empty() && view.history.back().number == pCompleted->record.number) {
                view.history.back() = pCompleted->record; // A regenerated turn replaces the last one.
            } else {
                view.history.push_back(pCompleted->record);
            }
            view.state = pCompleted->state;
            return StepOutcome::Completed;
        } else if (const auto* pOpened = std::get_if<turn::RunOpened>(&*event)) {
            std::cout << std::format("{} turn(s) played in world '{}'.{}\n", pOpened->history.size(),
                                     pOpened->meta.worldId,
                                     pOpened->worldChanged ? " The world's files changed since this run started." : "");
            if (!pOpened->history.empty()) {
                std::cout << '\n' << pOpened->history.back().narration << '\n';
            }
            view.history = pOpened->history;
            view.state = pOpened->state;
            return StepOutcome::Completed;
        } else if (const auto* pChanged = std::get_if<turn::StateChanged>(&*event)) {
            std::cout << std::format("Back to turn {}.\n", pChanged->history.size());
            view.history = pChanged->history;
            view.state = pChanged->state;
            return StepOutcome::Completed;
        } else if (std::holds_alternative<turn::TurnCancelled>(*event)) {
            std::cout << "\n[cancelled]\n";
            return StepOutcome::Cancelled;
        } else if (const auto* pError = std::get_if<turn::Error>(&*event)) {
            std::cerr << "error: " << pError->message << '\n';
            return StepOutcome::Failed;
        } else {
            // Every event the worker sends is handled above; a new one must be handled here too.
            MORPHEUS_REQUIRE(false, "morpheus_play: unhandled worker event, variant index " + std::to_string(event->index()));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::cerr << USAGE;
        return 2;
    }
    std::optional<std::uint64_t> seed;
    std::optional<std::filesystem::path> recordFile;
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            // A bad, negative or overflowing value is a usage error, not an exception or a wrapped number.
            const std::string_view text = argv[++i];
            std::uint64_t value = 0;
            const auto [pEnd, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || pEnd != text.data() + text.size()) {
                std::cerr << USAGE;
                return 2;
            }
            seed = value;
        } else if (arg == "--record" && i + 1 < argc) {
            recordFile = argv[++i];
        } else {
            std::cerr << USAGE;
            return 2;
        }
    }

    try {
        const core::Settings settings = core::LoadSettings(argv[1]);
        const std::filesystem::path worldFolder = argv[2];
        const std::filesystem::path runFile = argv[3];
        const bool resume = std::filesystem::exists(runFile);
        if (resume && recordFile) {
            std::cerr << "--record needs a new run file\n";
            return 2;
        }
        // The fixture stores the world path as given, and the replay test resolves it from the repo root. A drive or
        // a leading slash makes a path non-portable even when it is not "absolute" on this platform.
        if (recordFile && (worldFolder.has_root_name() || worldFolder.has_root_directory())) {
            std::cerr << "--record needs a world folder path relative to the repo root (run from the repo root), "
                         "not a rooted or absolute path: the fixture stores the path as given\n";
            return 2;
        }
        if (recordFile && !std::filesystem::is_directory(worldFolder)) {
            std::cerr << "--record needs the world folder '" << core::PathText(worldFolder)
                      << "' to exist relative to the current directory: run morpheus_play from the repo root\n";
            return 2;
        }

        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend llamaBackend(config);
        std::optional<turn::RecordingBackend> recording;
        if (recordFile) {
            recording.emplace(llamaBackend);
        }
        inference::LlmBackend& backend =
            recording ? static_cast<inference::LlmBackend&>(*recording) : static_cast<inference::LlmBackend&>(llamaBackend);

        std::random_device entropy;
        const std::uint64_t runSeed =
            seed.value_or((static_cast<std::uint64_t>(entropy()) << 32) | static_cast<std::uint64_t>(entropy()));
        RunView view;
        std::vector<turn::ReplayStep> steps;
        {
            turn::Worker worker(settings, backend);
            if (resume) {
                worker.Post(turn::LoadRun{worldFolder, runFile});
            } else {
                worker.Post(turn::StartRun{worldFolder, runFile, runSeed});
            }
            if (PrintUntilClosing(worker, view) != StepOutcome::Completed) {
                return 1;
            }

            std::string line;
            while ((std::cout << "\n> " << std::flush) && std::getline(std::cin, line)) {
                line = core::Trim(line);
                if (line.empty()) {
                    continue;
                }
                if (line == "/quit") {
                    break;
                }
                if (line == "/state") {
                    std::cout << context::RenderStateView(view.state);
                    continue;
                }
                // The mark comes before the post: maintenance of the previous turn may still be calling the backend on
                // the worker thread, and a reply it records before the mark belongs to the earlier, committed step.
                // One it records after the mark is a summary, which RollBack keeps (summaries never break a replay).
                const std::map<core::CallKind, std::size_t> mark =
                    recording ? recording->Mark() : std::map<core::CallKind, std::size_t>{};
                turn::ReplayStep step;
                if (line == "/undo") {
                    step.action = turn::ReplayStep::Action::Undo;
                    worker.Post(turn::UndoTurn{});
                } else if (line == "/regen") {
                    step.action = turn::ReplayStep::Action::Regenerate;
                    worker.Post(turn::RegenerateTurn{});
                } else {
                    step.input = line;
                    worker.Post(turn::SubmitInput{line});
                }
                std::cout << '\n';
                // A step that does not complete changes nothing, so a replay must see neither it nor its replies.
                if (PrintUntilClosing(worker, view) == StepOutcome::Completed) {
                    steps.push_back(std::move(step));
                } else if (recording) {
                    recording->RollBack(mark);
                }
            }

            // Maintenance of the last turn runs after that turn's closing event, so its outcome may still be
            // queued. Take what has been reported before the worker goes away; maintenance still running now is
            // cancelled by the shutdown, and its cancelled summary calls are not recorded.
            for (const turn::Event& event : worker.PollEvents()) {
                if (const auto* pFailed = std::get_if<turn::MaintenanceFailed>(&event)) {
                    std::cerr << "maintenance: " << pFailed->message << '\n';
                    view.maintenanceFailed = true;
                }
            }
        }

        if (recordFile) {
            // The replay answers embeddings with scripted vectors, so it would not reproduce a maintenance failure
            // caused by the real models; the recording is refused because its maintenance cannot be trusted.
            if (view.maintenanceFailed) {
                std::cerr << "error: maintenance failed during this session, so the recording is refused: its "
                             "maintenance cannot be trusted. The fixture was not written.\n";
                return 1;
            }
            const core::MemoryMode mode = settings.memoryMode.value_or(core::LoadWorld(worldFolder).defaultMemoryMode);
            std::ofstream out(*recordFile, std::ios::binary);
            out << turn::MakeReplayFixture(core::PathText(worldFolder), runSeed, mode, steps, *recording, view.history,
                                           view.state)
                       .dump(2)
                << '\n';
            if (!out) {
                throw std::runtime_error("cannot write " + core::PathText(*recordFile));
            }
            std::cout << "Wrote " << core::PathText(*recordFile) << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
