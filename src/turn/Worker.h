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

// Events (worker to UI). Every command ends with exactly one closing event (IsClosing):
// - StartRun and LoadRun: RunOpened or Error.
// - SubmitInput and RegenerateTurn: TurnCompleted, TurnCancelled or Error.
// - UndoTurn: StateChanged or Error.
// Any command cancelled before the worker picks it up closes with TurnCancelled without running, StartRun, LoadRun
// and UndoTurn included. MemoryUpdated and MaintenanceFailed report between-turn upkeep and close nothing.
// The run's memory as the Memory window shows it: every summary and every note, oldest first.
struct MemoryView {
    std::vector<core::Summary> summaries = {};
    std::vector<core::Note> notes = {};
};
struct RunOpened {
    store::RunMeta meta = {};
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
    MemoryView memory = {};
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
    MemoryView memory = {};
    context::Assembly assembly = {}; // The resolve prompt, for the context inspector.
};
struct TurnCancelled {};
struct StateChanged {
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
    MemoryView memory = {};
};
// Between-turn maintenance finished: it may have folded turns into the summary and indexed new notes. Closes nothing.
struct MemoryUpdated {
    MemoryView memory = {};
};
struct MaintenanceFailed {
    std::string message = {};
};
// A command failed. A failed StartRun or LoadRun has already closed the previous run, so no run is open after it.
struct Error {
    std::string message = {};
};
using Event = std::variant<RunOpened, TokenStreamed, TurnResolved, TurnCompleted, TurnCancelled, StateChanged,
                           MemoryUpdated, MaintenanceFailed, Error>;

// Whether `event` ends the command it belongs to.
bool IsClosing(const Event& event);

// The engine worker thread (architecture spec, "Process and modules"): the only thread that touches the backend
// and the open run, which it also closes on exit. Commands run one at a time in order. A failed StartRun or
// LoadRun leaves no run open.
//
// Maintenance (Session::Maintain) falls due when a turn commits and stays due until it finishes; it folds the
// transcript and indexes the latest turns and notes, so retrieval needs it done.
// - While no command waits, the worker runs it. A command posted meanwhile preempts it, so the UI stays responsive;
//   it stays due.
// - Before a SubmitInput or RegenerateTurn (the commands that retrieve), a due maintenance runs first, and that
//   command does not preempt it. So a turn always sees the previous turn's notes, even when it was queued early.
// - Other commands (StartRun, LoadRun, UndoTurn) run before a due maintenance. Opening a run (or failing to)
//   replaces the session, which clears it.
// - An explicit Cancel stops it; it then stays due only if a command waits.
class Worker {
private:
    struct Posted {
        std::uint64_t number = 0; // Post order, from 1.
        Command command = {};
    };

    core::Settings _settings = {};
    inference::LlmBackend* _pBackend = nullptr; // Not owned; used only on the worker thread.
    std::mutex _mutex = {};
    std::condition_variable_any _commandPosted = {};
    std::condition_variable _eventPosted = {};
    std::deque<Posted> _commands = {};
    std::deque<Event> _events = {};
    std::uint64_t _postedThrough = 0;     // The number of the latest posted command.
    std::uint64_t _cancelledThrough = 0;  // Commands numbered up to this were cancelled; queued ones never run.
    std::stop_source _jobStop = {};       // The running job's; Cancel and shutdown request it.
    bool _maintaining = false;            // The running job is maintenance that a new command preempts.
    std::optional<Session> _session = {}; // Worker thread only.
    std::jthread _thread = {};            // Last: starts after every other member exists, and is joined first.

public:
    // Starts the worker thread. `backend` must outlive the worker.
    Worker(core::Settings settings, inference::LlmBackend& backend);
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void Post(Command command);

    // Cancels every command posted so far and any running maintenance. A queued command closes with TurnCancelled
    // without running. A running SubmitInput or RegenerateTurn stops and closes with TurnCancelled; a running
    // StartRun, LoadRun or UndoTurn does not watch the token and ends with its usual event. No effect when idle.
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
    // The open run's summaries and notes, for the events that carry them.
    MemoryView Memory();
    void Emit(Event event);
};

} // namespace morpheus::turn
