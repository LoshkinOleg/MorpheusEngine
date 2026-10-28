#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/Session.h"

namespace morpheus::turn {

// Commands (UI to worker). Cancel is not a command: Worker::Cancel interrupts the running one.
struct StartRun {
    std::filesystem::path worldFolder = {};
    std::filesystem::path runFile = {};
    std::uint64_t seed = 0;
};
struct LoadRun {
    std::filesystem::path worldFolder = {};
    std::filesystem::path runFile = {};
};
struct SubmitInput {
    std::string text = {};
};
struct UndoTurn {};
struct RegenerateTurn {};
using Command = std::variant<StartRun, LoadRun, SubmitInput, UndoTurn, RegenerateTurn>;

// Events (worker to UI). Every command ends with exactly one closing event (IsClosing): RunOpened, TurnCompleted,
// TurnCancelled, StateChanged or Error. MaintenanceFailed reports between-turn upkeep and closes nothing.
struct RunOpened {
    store::RunMeta meta = {};
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
    bool worldChanged = false; // The world's files changed since the run started.
};
struct TokenStreamed {
    std::string piece = {};
};
struct TurnResolved {
    core::TurnRecord record = {}; // Checks and changes; narration still empty.
};
struct TurnCompleted {
    core::TurnRecord record = {};
    nlohmann::json state = nullptr;
    context::Assembly assembly = {}; // The resolve prompt, for the context inspector.
};
struct TurnCancelled {};
struct StateChanged {
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
};
struct MaintenanceFailed {
    std::string message = {};
};
struct Error {
    std::string message = {};
};
using Event = std::variant<RunOpened, TokenStreamed, TurnResolved, TurnCompleted, TurnCancelled, StateChanged,
                           MaintenanceFailed, Error>;

// Whether `event` ends the command it belongs to.
bool IsClosing(const Event& event);

// The engine worker thread (architecture spec, "Process and modules"): the only thread that touches the backend
// and the open run. Commands run one at a time in order. After a committed turn, while no command waits, the
// worker runs Session::Maintain; a new command preempts it.
class Worker {
private:
    core::Settings _settings;
    inference::LlmBackend* _pBackend = nullptr; // Not owned; used only on the worker thread.
    std::mutex _mutex = {};
    std::condition_variable_any _commandPosted = {};
    std::condition_variable _eventPosted = {};
    std::deque<Command> _commands = {};
    std::deque<Event> _events = {};
    std::stop_source _jobStop = {};       // The running job's; Cancel and shutdown request it.
    bool _maintaining = false;            // The running job is maintenance, which a new command preempts.
    std::optional<Session> _session = {}; // Worker thread only.
    std::jthread _thread = {};            // Last: starts after every other member exists, and is joined first.

public:
    // Starts the worker thread. `backend` must outlive the worker.
    Worker(core::Settings settings, inference::LlmBackend& backend);
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void Post(Command command);

    // Cancels the running command (it ends with TurnCancelled) or maintenance. No effect when idle.
    void Cancel();

    // Every event so far, oldest first. Never blocks: for the UI's frame loop.
    std::vector<Event> PollEvents();

    // The next event, waiting up to `timeout`.
    std::optional<Event> WaitEvent(std::chrono::milliseconds timeout);

private:
    void Run(std::stop_token shutdown);

    // Emits TurnCompleted or TurnCancelled for a turn's result. Returns whether it committed.
    bool FinishTurn(const std::optional<core::TurnRecord>& record);

    void EmitOpened();
    TurnCallbacks Callbacks();
    void Emit(Event event);
};

} // namespace morpheus::turn
