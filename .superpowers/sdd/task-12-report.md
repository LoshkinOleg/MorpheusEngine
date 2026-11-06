# Task 12 report: the engine worker thread

Status: DONE_WITH_CONCERNS (minor; see Concerns)

Commit: 03e8f2a Add the engine worker thread with command and event queues

## What was implemented
- `src/turn/Worker.h`, `src/turn/Worker.cpp`: verbatim from the brief. Commands (StartRun, LoadRun, SubmitInput,
  UndoTurn, RegenerateTurn), events (RunOpened, TokenStreamed, TurnResolved, TurnCompleted, TurnCancelled,
  StateChanged, MaintenanceFailed, Error), `IsClosing`, and `turn::Worker` with Post, Cancel, PollEvents, WaitEvent.
  The command executor is the local lambda `execute` inside `Run` (single caller, per CLAUDE.md).
- `src/turn/CMakeLists.txt`: added `Worker.cpp`.
- `tests/turn/WorkerTests.cpp`: the brief's four `[worker]` tests, verbatim.

## TDD evidence
RED: `cmake --build --preset tests`
```
WorkerTests.cpp(15,1): error C1083: Cannot open include file: 'turn/Worker.h': No such file or directory
```
GREEN: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[worker]"` (5 runs)
```
All tests passed (35 assertions in 4 test cases)   (x5)
```
- `ctest --preset tests`: 100% tests passed out of 179.
- ASan: `[worker]` 3 runs, all pass, no ASan reports; `ctest --preset asan`: 100% tests passed out of 179.
- `cmake --build --preset dev`: exit 0, no errors or warnings; `[worker]` also passes in the dev build.

Durations (Debug and ASan alike): the cancel and shutdown tests take ~1.05 s each, the turn test ~0.03-0.05 s.

## Files changed
- src/turn/Worker.h (new)
- src/turn/Worker.cpp (new)
- src/turn/CMakeLists.txt
- tests/turn/WorkerTests.cpp (new)

## Deviations from the brief
None. The code compiled cleanly under /W4 /WX with no adjustments.

## Data-race review
No data race found. Checked:
- `_commands`, `_events`, `_jobStop`, `_maintaining` are only touched under `_mutex`.
- `_session`, `_settings`, `_pBackend` are only used on the worker thread; `_session` is destroyed after the jthread
  is joined (`_thread` is the last member, so it is destroyed first), and join orders it.
- Shutdown: `std::stop_callback cancelOnShutdown` runs `Cancel` on the destroying thread. `request_stop` sets the
  stop state before running callbacks, and the worker checks `shutdown.stop_requested()` under the lock before it
  replaces `_jobStop`, so a shutdown is never lost: either the worker sees it and returns, or Cancel blocks on the
  lock until the new job's `_jobStop` exists and stops that one.
- No deadlock on exit: every `return` in `Run` is inside the lock block, and the `unique_lock` is destroyed before
  `cancelOnShutdown`, so the callback's destructor (which waits for a running Cancel) never holds `_mutex`.
- `_maintaining` stays true while idle after maintenance; Post then stops a stale `_jobStop`, which is harmless
  because the next job creates a new stop source.

## Concerns
- Timing margin of the shutdown test: it takes ~1.05 s against the 1.5 s limit. The time is dominated by the
  20 ms piece delay on the resolve reply (~33 pieces), and Windows' ~15.6 ms sleep granularity stretches each
  sleep to ~31 ms. Margin is ~0.45 s in both Debug and ASan; a heavily loaded machine could flake it. Without
  cancellation the narration alone would take ~3 s, so the test still discriminates well.
- Behavior notes (by design, not bugs): a Cancel that arrives after a command is posted but before the worker picks
  it up is lost ("no effect when idle"). Maintenance is skipped when a command is already queued after a committed
  turn, or when it is preempted; it next runs after the following committed turn. `Session::Maintain`'s return
  value is ignored.

## Fix round 1

Commit: 3702e60 "Keep maintenance due until it finishes, honor early cancels, fix the shutdown timing test".
Files: src/turn/Worker.h, src/turn/Worker.cpp, tests/turn/WorkerTests.cpp. ScriptedBackend unchanged.

### Important 1: shutdown test timing
- The start time is now taken right after the first TokenStreamed, just before the worker scope ends; the limit is
  500 ms.
- RED (test-only change, so shown by temporarily removing the shutdown stop_callback from Run, then restoring it):
  `morpheus_tests.exe "Destroying a busy worker cancels its job instead of waiting for it"` failed with
  `3092396300 ns < 500 ms`.
- GREEN: passes; the test takes ~1.04 s in total, almost all of it the resolve reply before streaming starts.

### Important 2: maintenance stays due until it finishes
- Result is ORed: `maintenanceDue = execute(...) || (maintenanceDue && !opensRun)`. StartRun/LoadRun clear it
  (the new run, or no run after a failure, has nothing due); commented in Run.
- Maintenance branch: Maintain's result is used. Finished or threw: cleared. Stopped early: stays due only if a
  command is queued (read under the lock), so a Cancel with nothing queued drops it and cannot restart it forever.
- Ordering: a due maintenance runs before a queued SubmitInput or RegenerateTurn. That pre-turn maintenance is not
  preemptible by Post (`_maintaining` is true only for maintenance run while nothing waits): preempting it would
  only restart it, since every later command waits behind the retrieving one. Cancel and shutdown still stop it.
  Idle maintenance is still preempted by Post. Documented in the Worker class comment.
- Test "A turn posted before the previous one commits still sees its notes" (assisted mode, 1 ms piece delay,
  both SubmitInputs posted back to back, Calls() read after the second closing event). Passes in either
  arrival order with the fix: if the second turn arrives late, idle maintenance either finishes or is preempted
  and then reruns before the turn.
- RED: the second resolve prompt had "# Relevant" with only lore hits, no
  "Note from turn 1: The keeper hums an old sea shanty.". GREEN: passes.

### Minor 1: lost Cancel
- Posted commands carry a sequence number (`Posted{number, command}`, `_postedThrough`); Cancel records
  `_cancelledThrough = _postedThrough`. A command picked up with number <= `_cancelledThrough` closes with
  TurnCancelled without running (any command kind; documented in the event comment).
- Test "A command cancelled before the worker picks it up closes with TurnCancelled".
- RED: the queued command ran in full (`queued.size() == 1` was `102 == 1`, closing event not TurnCancelled, and the
  following Undo did not fail because a turn was committed). GREEN: passes.

### Minor 2-6
- Cancel comment reworded: queued commands close with TurnCancelled; a running SubmitInput/RegenerateTurn stops;
  a running StartRun/LoadRun/UndoTurn ends with its usual event.
- Error event and class comment: a failed StartRun/LoadRun leaves no run open.
- Run exits through `break` and ends with `_session.reset()`, so the run is closed on the worker thread.
- MaintenanceFailed from a ContractError is prefixed "engine bug: ". Maintenance also asserts a run is open
  (MORPHEUS_REQUIRE) instead of silently skipping; unreachable since opening a run clears the flag. No test (no
  way to provoke a ContractError from Maintain with the fakes).
- `_settings = {}`; unused `turn/EngineText.h` include and `namespace fs` removed from the tests (added `<cstddef>`,
  `<thread>`, `<utility>` for the new tests).

### Minor 7: new [worker] tests (pass on first run: existing behavior)
- RegenerateTurn closes with TurnCompleted, record number 1, attempt 1.
- LoadRun of a run closed by destroying the previous worker: RunOpened with one turn and its state.
- PollEvents: empty and immediate on a fresh worker; after StartRun + SubmitInput, polling returns RunOpened,
  TurnResolved, the TokenStreamed pieces, TurnCompleted, in order, then nothing.

### Results
- `[worker]` under tests: 5 runs, all pass (9 test cases, 121 assertions). Durations: cancel tests and shutdown
  ~1.04 s, notes test ~1.25 s, others < 0.05 s.
- `[worker]` under asan: 3 runs, all pass, no ASan reports.
- `ctest --preset tests`: 100% passed out of 184. `ctest --preset asan`: 100% passed out of 184.

### Concerns
- The notes test takes ~1.25 s because Windows stretches each 1 ms piece sleep to ~15 ms. The delay is only there
  to make the "queued before commit" order likely; the test passes regardless of order.
- After an explicit Cancel during pre-turn maintenance, the cancelled turn closes and maintenance then runs once
  more while idle (it was still due with a command queued). Not a loop: a further Cancel with nothing queued drops it.
