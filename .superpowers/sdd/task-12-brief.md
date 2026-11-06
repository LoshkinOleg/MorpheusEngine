### Task 12: The engine worker thread

**Files:**
- Create: `src/turn/Worker.h`, `src/turn/Worker.cpp`, `tests/turn/WorkerTests.cpp`
- Modify: `src/turn/CMakeLists.txt` (add `Worker.cpp`)

**Interfaces:**
- Consumes: `Session`, `core::LoadWorld`.
- Produces:
  - Commands: `turn::StartRun{worldFolder, runFile, seed}`, `turn::LoadRun{worldFolder, runFile}`, `turn::SubmitInput{text}`, `turn::UndoTurn{}`, `turn::RegenerateTurn{}`, and `turn::Command` (a variant of these)
  - Events: `turn::RunOpened{meta, history, state, worldChanged}`, `turn::TokenStreamed{piece}`, `turn::TurnResolved{record}`, `turn::TurnCompleted{record, state, assembly}`, `turn::TurnCancelled{}`, `turn::StateChanged{history, state}`, `turn::MaintenanceFailed{message}`, `turn::Error{message}`, and `turn::Event` (a variant of these)
  - `turn::IsClosing(const Event&) -> bool`
  - `class turn::Worker(core::Settings, inference::LlmBackend&)`, with `Post(Command)`, `Cancel()`, `PollEvents()` and `WaitEvent(std::chrono::milliseconds)`

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/WorkerTests.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `turn/Worker.h` cannot be opened.

- [ ] **Step 3: Implement**

Create `src/turn/Worker.h`:

```cpp
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
```

Create `src/turn/Worker.cpp`:

```cpp
#include "turn/Worker.h"

#include <exception>
#include <string_view>
#include <utility>

#include "core/Errors.h"
#include "core/World.h"

namespace morpheus::turn {

bool IsClosing(const Event& event)
{
    return std::holds_alternative<RunOpened>(event) || std::holds_alternative<TurnCompleted>(event) ||
           std::holds_alternative<TurnCancelled>(event) || std::holds_alternative<StateChanged>(event) ||
           std::holds_alternative<Error>(event);
}

Worker::Worker(core::Settings settings, inference::LlmBackend& backend)
    : _settings(std::move(settings))
    , _pBackend(&backend)
    , _thread([this](std::stop_token shutdown) { Run(shutdown); })
{
}

void Worker::Post(Command command)
{
    {
        std::scoped_lock lock(_mutex);
        _commands.push_back(std::move(command));
        if (_maintaining) {
            _jobStop.request_stop(); // A command preempts between-turn maintenance.
        }
    }
    _commandPosted.notify_one();
}

void Worker::Cancel()
{
    std::scoped_lock lock(_mutex);
    _jobStop.request_stop();
}

std::vector<Event> Worker::PollEvents()
{
    std::scoped_lock lock(_mutex);
    std::vector<Event> events(std::make_move_iterator(_events.begin()), std::make_move_iterator(_events.end()));
    _events.clear();
    return events;
}

std::optional<Event> Worker::WaitEvent(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(_mutex);
    if (!_eventPosted.wait_for(lock, timeout, [this] { return !_events.empty(); })) {
        return std::nullopt;
    }
    Event event = std::move(_events.front());
    _events.pop_front();
    return event;
}

void Worker::Run(std::stop_token shutdown)
{
    // Runs one command and emits its events. Returns true when it committed a turn (maintenance is due).
    auto execute = [this](const Command& command, std::stop_token stop) {
        try {
            if (const auto* pStart = std::get_if<StartRun>(&command)) {
                _session.reset(); // Close the previous run first.
                _session.emplace(Session::Start(core::LoadWorld(pStart->worldFolder), _settings, pStart->runFile,
                                                pStart->seed, *_pBackend));
                EmitOpened();
                return false;
            }
            if (const auto* pLoad = std::get_if<LoadRun>(&command)) {
                _session.reset();
                _session.emplace(
                    Session::Load(core::LoadWorld(pLoad->worldFolder), _settings, pLoad->runFile, *_pBackend));
                EmitOpened();
                return false;
            }
            if (!_session) {
                throw TurnError("no run is open");
            }
            if (const auto* pInput = std::get_if<SubmitInput>(&command)) {
                return FinishTurn(_session->RunTurn(pInput->text, Callbacks(), stop));
            }
            if (std::holds_alternative<UndoTurn>(command)) {
                _session->Undo();
                Emit(StateChanged{_session->History(), _session->State()});
                return false;
            }
            return FinishTurn(_session->Regenerate(Callbacks(), stop));
        } catch (const core::ContractError& error) {
            Emit(Error{std::string("engine bug: ") + error.what()});
        } catch (const std::exception& error) {
            Emit(Error{error.what()});
        }
        return false;
    };

    // Shutdown cancels the running job too, so destroying the worker never waits for a whole turn.
    std::stop_callback cancelOnShutdown(shutdown, [this] { Cancel(); });
    bool maintenanceDue = false;
    for (;;) {
        std::optional<Command> command;
        std::stop_token stop;
        {
            std::unique_lock lock(_mutex);
            if (!maintenanceDue && !_commandPosted.wait(lock, shutdown, [this] { return !_commands.empty(); })) {
                return; // Shutdown while idle.
            }
            if (shutdown.stop_requested()) {
                return;
            }
            if (!_commands.empty()) {
                command = std::move(_commands.front());
                _commands.pop_front();
            }
            _jobStop = std::stop_source();
            _maintaining = !command.has_value();
            stop = _jobStop.get_token();
        }
        if (command) {
            maintenanceDue = execute(*command, stop);
        } else {
            try {
                if (_session) {
                    _session->Maintain(stop);
                }
            } catch (const std::exception& error) {
                Emit(MaintenanceFailed{error.what()});
            }
            maintenanceDue = false;
        }
    }
}

bool Worker::FinishTurn(const std::optional<core::TurnRecord>& record)
{
    if (!record) {
        Emit(TurnCancelled{});
        return false;
    }
    Emit(TurnCompleted{*record, _session->State(), _session->LastAssembly()});
    return true;
}

void Worker::EmitOpened()
{
    Emit(RunOpened{_session->Meta(), _session->History(), _session->State(), _session->WorldChanged()});
}

TurnCallbacks Worker::Callbacks()
{
    TurnCallbacks callbacks;
    callbacks.onResolved = [this](const core::TurnRecord& record) { Emit(TurnResolved{record}); };
    callbacks.onNarrationToken = [this](std::string_view piece) { Emit(TokenStreamed{std::string(piece)}); };
    return callbacks;
}

void Worker::Emit(Event event)
{
    {
        std::scoped_lock lock(_mutex);
        _events.push_back(std::move(event));
    }
    _eventPosted.notify_all();
}

} // namespace morpheus::turn
```

Notes:
- `_thread` is declared last, so it starts after the other members are built and is destroyed (joined) first. Its stop request runs `cancelOnShutdown`, and the condition-variable wait returns on `shutdown`.
- `Session` has no default constructor. `std::optional<Session>::emplace(Session&&)` uses its implicit move constructor.
- Add `Worker.cpp` to `src/turn/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[worker]"`
Expected: PASS.

Then run `ctest --preset tests` and `ctest --preset asan`. Expected: all pass. ASan also covers the thread hand-off.

- [ ] **Step 5: Commit**

```bash
git add src/turn tests/turn/WorkerTests.cpp
git commit -m "Add the engine worker thread with command and event queues" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
