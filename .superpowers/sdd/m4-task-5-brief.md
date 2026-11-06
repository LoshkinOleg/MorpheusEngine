### Task 5: The run model

`RunModel` turns the worker's event stream into what the windows draw: the committed history, the turn being played (input, then resolution, then streaming narration), the state, the memory, the last prompt and the last error. The UI runs one command at a time. `OnPosted` records the command, and its closing event ends it.

**Files:**
- Create: `src/app/model/RunModel.h`, `src/app/model/RunModel.cpp`, `tests/app/RunModelTests.cpp`
- Modify: `src/app/CMakeLists.txt` (add `model/RunModel.cpp`)

**Interfaces:**
- Consumes: the `turn::Command` and `turn::Event` types, `turn::MemoryView` (Task 1), `context::Assembly`, `core::TurnRecord`, `store::RunMeta`.
- Produces: `class app::RunModel` with:
  - the types `enum class Busy { None, Opening, Turn, Regenerate, Undo }` and `struct PendingTurn { std::string input; std::optional<core::TurnRecord> resolved; std::string narration; }`;
  - `void OnPosted(const turn::Command&)`, `void Apply(const turn::Event&)` and `void Close()`;
  - the accessors `bool Open() const`, `const store::RunMeta& Meta() const`, `bool WorldChanged() const`, `const std::vector<core::TurnRecord>& History() const`, `std::span<const core::TurnRecord> TranscriptTurns() const`, `const nlohmann::json& State() const` and `const turn::MemoryView& Memory() const`;
  - `const std::optional<context::Assembly>& LastAssembly() const`, `Busy CurrentBusy() const` and `const std::optional<PendingTurn>& Pending() const`;
  - `const std::string& LastError() const`, `const std::string& MaintenanceError() const`, `const core::TurnRecord* LatestTurn() const` and `std::optional<std::string> TakeRestoredInput()`.

- [ ] **Step 1: Write the failing tests**

Create `tests/app/RunModelTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/model/RunModel.h"
#include "core/Errors.h"
#include "turn/Worker.h"

using namespace morpheus;

namespace {

core::TurnRecord Record(int number, std::string input, std::string narration)
{
    core::TurnRecord record;
    record.number = number;
    record.input = std::move(input);
    record.narration = std::move(narration);
    return record;
}

// A model with a run open, as after LoadRun and its RunOpened.
app::RunModel OpenModel(std::vector<core::TurnRecord> history = {})
{
    app::RunModel model;
    model.OnPosted(turn::LoadRun{});
    turn::RunOpened opened;
    opened.meta.worldId = "mini";
    opened.history = std::move(history);
    opened.state = nlohmann::json::object();
    model.Apply(opened);
    return model;
}

} // namespace

TEST_CASE("RunModel follows a turn from input to narration", "[run_model]")
{
    app::RunModel model = OpenModel();
    REQUIRE(model.Open());
    CHECK(model.Meta().worldId == "mini");
    CHECK(model.LatestTurn() == nullptr);

    model.OnPosted(turn::SubmitInput{"I walk."});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Turn);
    REQUIRE(model.Pending());
    CHECK(model.Pending()->input == "I walk.");
    CHECK_FALSE(model.Pending()->resolved);

    model.Apply(turn::TurnResolved{Record(1, "I walk.", "")});
    CHECK(model.Pending()->resolved);
    model.Apply(turn::TokenStreamed{"You "});
    model.Apply(turn::TokenStreamed{"walk."});
    CHECK(model.Pending()->narration == "You walk.");

    turn::TurnCompleted completed;
    completed.record = Record(1, "I walk.", "You walk.");
    completed.state = {{"player", 1}};
    completed.memory.notes = {{1, "A note."}};
    completed.assembly.tokens.total = 123;
    model.Apply(completed);
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);
    CHECK_FALSE(model.Pending());
    REQUIRE(model.History().size() == 1);
    CHECK(model.LatestTurn()->narration == "You walk.");
    CHECK(model.TranscriptTurns().size() == 1);
    CHECK(model.State().at("player") == 1);
    CHECK(model.Memory().notes.size() == 1);
    REQUIRE(model.LastAssembly());
    CHECK(model.LastAssembly()->tokens.total == 123);
}

TEST_CASE("A regenerate replaces the latest turn and hides it while it runs", "[run_model]")
{
    app::RunModel model = OpenModel({Record(1, "I walk.", "Old.")});
    model.OnPosted(turn::RegenerateTurn{});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Regenerate);
    CHECK(model.Pending()->input == "I walk."); // The latest turn's input, played again.
    CHECK(model.TranscriptTurns().empty());     // The old text is hidden while the new one streams.

    core::TurnRecord again = Record(1, "I walk.", "New.");
    again.attempt = 1;
    model.Apply(turn::TurnCompleted{again, nlohmann::json::object(), {}, {}});
    REQUIRE(model.History().size() == 1);
    CHECK(model.History()[0].narration == "New.");
    CHECK(model.History()[0].attempt == 1);
    CHECK(model.TranscriptTurns().size() == 1);
}

TEST_CASE("A cancelled or failed turn commits nothing and offers its input again", "[run_model]")
{
    app::RunModel model = OpenModel();
    model.OnPosted(turn::SubmitInput{"I jump."});
    model.Apply(turn::TokenStreamed{"You"});
    model.Apply(turn::TurnCancelled{});
    CHECK(model.History().empty());
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);
    CHECK_FALSE(model.Pending());
    CHECK(model.TakeRestoredInput() == std::optional<std::string>("I jump."));
    CHECK_FALSE(model.TakeRestoredInput()); // Offered once.

    model.OnPosted(turn::SubmitInput{"I fly."});
    model.Apply(turn::Error{"input too long"});
    CHECK(model.LastError() == "input too long");
    CHECK(model.TakeRestoredInput() == std::optional<std::string>("I fly."));
    CHECK(model.Open()); // A failed turn leaves the run open.

    model.OnPosted(turn::SubmitInput{"I walk."});
    CHECK(model.LastError().empty()); // Cleared by the next command.
}

TEST_CASE("A failed open closes the previous run", "[run_model]")
{
    app::RunModel model = OpenModel({Record(1, "I walk.", "You walk.")});
    model.OnPosted(turn::LoadRun{});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Opening);
    model.Apply(turn::Error{"run belongs to another world"});
    CHECK_FALSE(model.Open());
    CHECK(model.History().empty());
    CHECK(model.LastError() == "run belongs to another world");
}

TEST_CASE("Undo takes the history and state the worker reports", "[run_model]")
{
    app::RunModel model = OpenModel({Record(1, "I walk.", "You walk.")});
    model.OnPosted(turn::UndoTurn{});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Undo);
    model.Apply(turn::StateChanged{{}, nlohmann::json::object(), {}});
    CHECK(model.History().empty());
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);
    CHECK_FALSE(model.LastAssembly());
}

TEST_CASE("Maintenance reports update the memory between commands", "[run_model]")
{
    app::RunModel model = OpenModel();
    turn::MemoryUpdated updated;
    updated.memory.summaries = {{1, 2, "The keeper's story."}};
    model.Apply(updated);
    CHECK(model.Memory().summaries.size() == 1);
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);

    model.Apply(turn::MaintenanceFailed{"summary_cap too small"});
    CHECK(model.MaintenanceError() == "summary_cap too small");
    model.Apply(updated);
    CHECK(model.MaintenanceError().empty()); // It finished since.
}

TEST_CASE("RunModel rejects commands and events the UI cannot produce", "[run_model]")
{
    app::RunModel closed;
    CHECK_THROWS_AS(closed.OnPosted(turn::SubmitInput{"I walk."}), core::ContractError); // No run open.
    CHECK_THROWS_AS(closed.Apply(turn::TurnCancelled{}), core::ContractError);           // No command running.

    app::RunModel model = OpenModel();
    CHECK_THROWS_AS(model.OnPosted(turn::UndoTurn{}), core::ContractError); // No turn to undo.
    CHECK_THROWS_AS(model.Apply(turn::TokenStreamed{"x"}), core::ContractError);
    model.OnPosted(turn::SubmitInput{"I walk."});
    CHECK_THROWS_AS(model.OnPosted(turn::SubmitInput{"I run."}), core::ContractError); // One at a time.
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Add `model/RunModel.cpp` to `src/app/CMakeLists.txt`.

Run: `cmake --build --preset tests`
Expected: a compile error: `app/model/RunModel.h` is missing.

- [ ] **Step 3: Write the header**

Create `src/app/model/RunModel.h`:

```cpp
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/TurnRecord.h"
#include "store/RunStore.h"
#include "turn/Worker.h"

namespace morpheus::app {

// The open run as the windows show it, kept up to date from the worker's events (UI thread only). The UI runs one
// command at a time: OnPosted records it, and its closing event (turn::IsClosing) ends it. MemoryUpdated and
// MaintenanceFailed arrive between commands.
class RunModel {
public:
    // The command the worker is running for the UI.
    enum class Busy { None, Opening, Turn, Regenerate, Undo };

    // The turn being played: its input, then its resolved record (checks and changes), then the narration so far.
    struct PendingTurn {
        std::string input = {};
        std::optional<core::TurnRecord> resolved = {};
        std::string narration = {};
    };

private:
    bool _open = false;
    store::RunMeta _meta = {};
    bool _worldChanged = false;
    std::vector<core::TurnRecord> _history = {};
    nlohmann::json _state = nullptr;
    turn::MemoryView _memory = {};
    std::optional<context::Assembly> _lastAssembly = {}; // The latest turn's resolve prompt, if played this session.
    Busy _busy = Busy::None;
    std::optional<PendingTurn> _pending = {};
    std::string _lastError = {};        // Why the last command failed; cleared when the next one is posted.
    std::string _maintenanceError = {}; // Why maintenance last failed; cleared when it next finishes.
    std::optional<std::string> _restoredInput = {};

public:
    // Records `command` as posted to the worker. Throws ContractError while another command runs, for a turn
    // command with no run open, and for UndoTurn or RegenerateTurn with no turn.
    void OnPosted(const turn::Command& command);

    // Applies one worker event. Throws ContractError for an event the running command cannot produce (a bug).
    void Apply(const turn::Event& event);

    // Forgets the run, as when the engine restarts: the worker that had it open is gone.
    void Close();

    bool Open() const;
    const store::RunMeta& Meta() const;
    bool WorldChanged() const;
    const std::vector<core::TurnRecord>& History() const;
    // The committed turns the transcript shows: all of them, except the latest while it is being regenerated.
    std::span<const core::TurnRecord> TranscriptTurns() const;
    const nlohmann::json& State() const;
    const turn::MemoryView& Memory() const;
    const std::optional<context::Assembly>& LastAssembly() const;
    Busy CurrentBusy() const;
    const std::optional<PendingTurn>& Pending() const;
    const std::string& LastError() const;
    const std::string& MaintenanceError() const;
    // The latest committed turn, or nullptr before the first.
    const core::TurnRecord* LatestTurn() const;
    // The input of a turn that was cancelled or failed, once, so the input box can offer it again.
    std::optional<std::string> TakeRestoredInput();
};

} // namespace morpheus::app
```

- [ ] **Step 4: Write the implementation**

Create `src/app/model/RunModel.cpp`:

```cpp
#include "app/model/RunModel.h"

#include <utility>
#include <variant>

#include "core/Errors.h"

namespace morpheus::app {

void RunModel::OnPosted(const turn::Command& command)
{
    MORPHEUS_REQUIRE(_busy == Busy::None, "RunModel: a command is already running");
    _lastError.clear();
    if (std::holds_alternative<turn::StartRun>(command) || std::holds_alternative<turn::LoadRun>(command)) {
        _busy = Busy::Opening;
        return;
    }
    MORPHEUS_REQUIRE(_open, "RunModel: a turn command needs an open run");
    if (const auto* pInput = std::get_if<turn::SubmitInput>(&command)) {
        _busy = Busy::Turn;
        _pending = PendingTurn{pInput->text};
        return;
    }
    MORPHEUS_REQUIRE(!_history.empty(), "RunModel: undo and regenerate need a committed turn");
    if (std::holds_alternative<turn::RegenerateTurn>(command)) {
        _busy = Busy::Regenerate;
        _pending = PendingTurn{_history.back().input}; // The same input, played again.
        return;
    }
    _busy = Busy::Undo;
}

void RunModel::Apply(const turn::Event& event)
{
    // Progress of the running turn.
    if (const auto* pResolved = std::get_if<turn::TurnResolved>(&event)) {
        MORPHEUS_REQUIRE(_pending.has_value(), "RunModel: TurnResolved with no turn running");
        _pending->resolved = pResolved->record;
        return;
    }
    if (const auto* pPiece = std::get_if<turn::TokenStreamed>(&event)) {
        MORPHEUS_REQUIRE(_pending.has_value(), "RunModel: TokenStreamed with no turn running");
        _pending->narration += pPiece->piece;
        return;
    }
    // Maintenance reports, between commands.
    if (const auto* pUpdated = std::get_if<turn::MemoryUpdated>(&event)) {
        _memory = pUpdated->memory;
        _maintenanceError.clear();
        return;
    }
    if (const auto* pFailed = std::get_if<turn::MaintenanceFailed>(&event)) {
        _maintenanceError = pFailed->message;
        return;
    }

    // Every other event closes the running command.
    MORPHEUS_REQUIRE(_busy != Busy::None, "RunModel: a closing event with no command running");
    const Busy finished = _busy;
    _busy = Busy::None;
    std::optional<PendingTurn> pending = std::move(_pending);
    _pending.reset();

    if (const auto* pOpened = std::get_if<turn::RunOpened>(&event)) {
        _open = true;
        _meta = pOpened->meta;
        _worldChanged = pOpened->worldChanged;
        _history = pOpened->history;
        _state = pOpened->state;
        _memory = pOpened->memory;
        _lastAssembly.reset(); // The prompt shows again once a turn is played.
        _maintenanceError.clear();
        return;
    }
    if (const auto* pCompleted = std::get_if<turn::TurnCompleted>(&event)) {
        if (finished == Busy::Regenerate) {
            MORPHEUS_REQUIRE(!_history.empty() && _history.back().number == pCompleted->record.number,
                             "RunModel: a regenerated turn must replace the latest turn");
            _history.back() = pCompleted->record;
        } else {
            MORPHEUS_REQUIRE(finished == Busy::Turn &&
                                 pCompleted->record.number == static_cast<int>(_history.size()) + 1,
                             "RunModel: a new turn must follow the latest turn");
            _history.push_back(pCompleted->record);
        }
        _state = pCompleted->state;
        _memory = pCompleted->memory;
        _lastAssembly = pCompleted->assembly;
        return;
    }
    if (const auto* pChanged = std::get_if<turn::StateChanged>(&event)) {
        _history = pChanged->history;
        _state = pChanged->state;
        _memory = pChanged->memory;
        _lastAssembly.reset(); // It was the prompt of a turn that is gone.
        return;
    }

    // TurnCancelled or Error: nothing was committed. A submitted input goes back to the input box.
    if (pending && finished == Busy::Turn) {
        _restoredInput = std::move(pending->input);
    }
    if (const auto* pError = std::get_if<turn::Error>(&event)) {
        if (finished == Busy::Opening) {
            Close(); // A failed StartRun or LoadRun has already closed the previous run.
        }
        _lastError = pError->message;
    }
}

void RunModel::Close()
{
    *this = RunModel();
}

bool RunModel::Open() const
{
    return _open;
}

const store::RunMeta& RunModel::Meta() const
{
    return _meta;
}

bool RunModel::WorldChanged() const
{
    return _worldChanged;
}

const std::vector<core::TurnRecord>& RunModel::History() const
{
    return _history;
}

std::span<const core::TurnRecord> RunModel::TranscriptTurns() const
{
    // OnPosted guarantees a turn to hide while regenerating.
    const std::size_t shown = _busy == Busy::Regenerate ? _history.size() - 1 : _history.size();
    return std::span<const core::TurnRecord>(_history.data(), shown);
}

const nlohmann::json& RunModel::State() const
{
    return _state;
}

const turn::MemoryView& RunModel::Memory() const
{
    return _memory;
}

const std::optional<context::Assembly>& RunModel::LastAssembly() const
{
    return _lastAssembly;
}

RunModel::Busy RunModel::CurrentBusy() const
{
    return _busy;
}

const std::optional<RunModel::PendingTurn>& RunModel::Pending() const
{
    return _pending;
}

const std::string& RunModel::LastError() const
{
    return _lastError;
}

const std::string& RunModel::MaintenanceError() const
{
    return _maintenanceError;
}

const core::TurnRecord* RunModel::LatestTurn() const
{
    return _history.empty() ? nullptr : &_history.back();
}

std::optional<std::string> RunModel::TakeRestoredInput()
{
    return std::exchange(_restoredInput, std::nullopt);
}

} // namespace morpheus::app
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[run_model]"`
Expected: all seven tests pass. Then `ctest --preset tests`: everything passes.

- [ ] **Step 6: Commit**

```bash
git add src/app/CMakeLists.txt src/app/model/RunModel.h src/app/model/RunModel.cpp tests/app/RunModelTests.cpp
git commit -m "Track the open run from worker events" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
