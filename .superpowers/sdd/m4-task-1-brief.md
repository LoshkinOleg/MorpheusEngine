### Task 1: Worker events carry the run's memory

The Memory window shows summaries and notes. Today the worker's events carry neither, and maintenance changes the summaries without any event. This task adds a `MemoryView` to the events that already carry state, and a non-closing `MemoryUpdated` event after maintenance finishes.

**Files:**
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`, `src/turn/Worker.h`, `src/turn/Worker.cpp`
- Modify: `tests/turn/WorkerTests.cpp`
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md` ("Process and modules")

**Interfaces:**
- Consumes: `store::RunStore::Notes()`, `Session::Summaries()`.
- Produces:
  - `struct turn::MemoryView { std::vector<core::Summary> summaries; std::vector<core::Note> notes; }`.
  - The field `MemoryView memory` on `RunOpened` (after `state`), `TurnCompleted` (after `state`, before `assembly`) and `StateChanged` (after `state`).
  - `struct turn::MemoryUpdated { MemoryView memory; }`, a new `Event` alternative placed after `StateChanged`; `IsClosing` is false for it.
  - `std::vector<core::Note> Session::Notes()`.

- [ ] **Step 1: Write the failing test**

Append to `tests/turn/WorkerTests.cpp`:

```cpp
TEST_CASE("Worker events carry the run's memory, and maintenance reports it", "[worker]")
{
    // Assisted mode: the lookup call asks for nothing, and the resolve output writes one note.
    const std::string resolveWithNote = R"({"intent":"walk","checks":[],"changes":[],"notes":["The keeper hums."]})";
    test::ScriptedBackend backend(16384, [&](const inference::Prompt&, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("lookups") != std::string::npos) {
            return std::string(R"({"lookups":[]})");
        }
        return spec.grammar.find("intent") != std::string::npos ? resolveWithNote : std::string("You walk.");
    });
    core::Settings settings = TestSettings();
    settings.memoryMode = core::MemoryMode::Assisted;
    turn::Worker worker(settings, backend);

    worker.Post(Start("worker_memory"));
    const std::vector<turn::Event> opened = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::RunOpened>(opened.back()));
    CHECK(std::get<turn::RunOpened>(opened.back()).memory.notes.empty());

    worker.Post(turn::SubmitInput{"I walk to the lighthouse."});
    const std::vector<turn::Event> events = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(events.back()));
    const turn::MemoryView& memory = std::get<turn::TurnCompleted>(events.back()).memory;
    REQUIRE(memory.notes.size() == 1);
    CHECK(memory.notes[0].turn == 1);
    CHECK(memory.notes[0].text == "The keeper hums.");

    // Maintenance runs while the worker is idle and reports the memory when it finishes. It closes nothing.
    const std::optional<turn::Event> maintained = worker.WaitEvent(std::chrono::seconds(10));
    REQUIRE(maintained);
    REQUIRE(std::holds_alternative<turn::MemoryUpdated>(*maintained));
    CHECK_FALSE(turn::IsClosing(*maintained));
    CHECK(std::get<turn::MemoryUpdated>(*maintained).memory.notes.size() == 1);

    worker.Post(turn::UndoTurn{});
    const std::vector<turn::Event> undone = UntilClosing(worker);
    REQUIRE(std::holds_alternative<turn::StateChanged>(undone.back()));
    CHECK(std::get<turn::StateChanged>(undone.back()).memory.notes.empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[worker]"`
Expected: a compile error: `memory` is not a member of `RunOpened`, and `MemoryView` and `MemoryUpdated` are not members of `morpheus::turn`.

- [ ] **Step 3: Add Session::Notes**

In `src/turn/Session.h`, below `std::vector<core::Summary> Summaries();`:

```cpp
    std::vector<core::Note> Notes(); // Every committed note, by turn, then the order written.
```

In `src/turn/Session.cpp`, after `Session::Summaries`:

```cpp
std::vector<core::Note> Session::Notes()
{
    return _store.Notes();
}
```

- [ ] **Step 4: Add the memory to the events**

In `src/turn/Worker.h`, above `struct RunOpened`, add:

```cpp
// The run's memory as the Memory window shows it: every summary and every note, oldest first.
struct MemoryView {
    std::vector<core::Summary> summaries = {};
    std::vector<core::Note> notes = {};
};
```

Change the three events and add `MemoryUpdated`:

```cpp
struct RunOpened {
    store::RunMeta meta = {};
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
    MemoryView memory = {};
    bool worldChanged = false; // The world's files changed since the run started.
};
```

```cpp
struct TurnCompleted {
    core::TurnRecord record = {};
    nlohmann::json state = nullptr;
    MemoryView memory = {};
    context::Assembly assembly = {}; // The resolve prompt, for the context inspector.
};
```

```cpp
struct StateChanged {
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
    MemoryView memory = {};
};
// Between-turn maintenance finished: it may have folded turns into the summary and indexed new notes. Closes nothing.
struct MemoryUpdated {
    MemoryView memory = {};
};
```

```cpp
using Event = std::variant<RunOpened, TokenStreamed, TurnResolved, TurnCompleted, TurnCancelled, StateChanged,
                           MemoryUpdated, MaintenanceFailed, Error>;
```

Change the event comment's last sentence from "MaintenanceFailed reports between-turn upkeep and closes nothing." to:

```cpp
// and UndoTurn included. MemoryUpdated and MaintenanceFailed report between-turn upkeep and close nothing.
```

In the class's private methods, after `TurnCallbacks Callbacks();`, add:

```cpp
    // The open run's summaries and notes, for the events that carry them.
    MemoryView Memory();
```

- [ ] **Step 5: Emit the memory**

In `src/turn/Worker.cpp`:

The undo branch of `execute`:

```cpp
            if (std::holds_alternative<UndoTurn>(command)) {
                _session->Undo();
                Emit(StateChanged{_session->History(), _session->State(), Memory()});
                return false;
            }
```

The maintenance branch of the loop. The report goes out only when maintenance finished: a stopped one runs again later and reports then.

```cpp
            try {
                MORPHEUS_REQUIRE(_session.has_value(), "Worker: maintenance is due with no run open");
                finished = _session->Maintain(stop);
                if (finished) {
                    Emit(MemoryUpdated{Memory()}); // Folding may have changed the summaries.
                }
            } catch (const core::ContractError& error) {
```

`FinishTurn` and `EmitOpened`:

```cpp
    Emit(TurnCompleted{*record, _session->State(), Memory(), _session->LastAssembly()});
```

```cpp
    Emit(RunOpened{_session->Meta(), _session->History(), _session->State(), Memory(), _session->WorldChanged()});
```

After `Worker::Callbacks`:

```cpp
MemoryView Worker::Memory()
{
    return MemoryView{_session->Summaries(), _session->Notes()};
}
```

- [ ] **Step 6: Run the worker tests and fix the one that polls past the last command**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[worker]"`

The new test passes. "PollEvents returns the queued events in order without blocking" can now fail: maintenance runs right after the turn commits, so a `MemoryUpdated` may arrive in the same poll as `TurnCompleted`, or after it. Replace that test's collection loop and the checks after it with:

```cpp
    std::vector<turn::Event> events;
    int closing = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (closing < 2) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        for (turn::Event& event : worker.PollEvents()) {
            if (closing == 2) {
                // After the last command closes, only maintenance reports.
                CHECK(std::holds_alternative<turn::MemoryUpdated>(event));
                continue;
            }
            closing += turn::IsClosing(event) ? 1 : 0;
            events.push_back(std::move(event));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Polling drained the queue; maintenance's report may still come after it.
    for (const turn::Event& late : worker.PollEvents()) {
        CHECK(std::holds_alternative<turn::MemoryUpdated>(late));
    }
```

The rest of the test (from `// Both commands' events, in the order they were emitted.`) stays. Run `[worker]` again: every test passes. If another existing test fails because a `MemoryUpdated` now arrives between commands, filter it the same way and say so in the report. Do not weaken any other check.

- [ ] **Step 7: Run the whole suite and update the spec**

Run: `ctest --preset tests`
Expected: all tests pass. `morpheus_play` reads events with `std::get_if` chains, so it ignores `MemoryUpdated` without changes.

In the architecture spec's "Process and modules", change the events bullet and the closing-event bullet to:

```markdown
- **Events (worker to UI):** `RunOpened`, `TokenStreamed`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MemoryUpdated`, `MaintenanceFailed`, `Error`. `RunOpened`, `TurnCompleted` and `StateChanged` carry the run's memory (summaries and notes) for the Memory window.
- Every command ends with exactly one closing event. `MemoryUpdated` (maintenance finished) and `MaintenanceFailed` report between-turn upkeep and close nothing.
```

- [ ] **Step 8: Commit**

```bash
git add src/turn/Session.h src/turn/Session.cpp src/turn/Worker.h src/turn/Worker.cpp tests/turn/WorkerTests.cpp docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
git commit -m "Report the run's memory in worker events" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

