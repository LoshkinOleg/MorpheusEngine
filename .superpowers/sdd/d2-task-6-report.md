# D2 Task 6 report: server::RunWorker

Status: DONE_WITH_CONCERNS (two deviations from the brief, both explained below; all tests pass)

Commit: a1f9971 RunWorker: a run's own thread, job queue and session, folding in the background between jobs

## What was implemented

- `src/server/RunWorker.h` / `.cpp`: the brief's public interface verbatim (Job, constructor, RunId, Post, Call,
  Reopen, RequestStop, private Enqueue). The worker opens the run on its own std::jthread (the constructor waits and
  rethrows Open's error), runs jobs one at a time in arrival order, runs HostedSession::Maintain on
  SlotPool::UPKEEP_SLOT while the queue is empty (a new job or RequestStop stops it through a per-run stop_source; it
  runs again after the next job), reopens with changed host data (checks with HostedWorld first; same hash is a no-op;
  a failed open reopens with the previous data and rethrows; a failed fallback leaves the run closed and logged), and
  stops (queued and later jobs fail with InferenceError "the server is stopping"; the running job's stop token fires).
- `src/server/CMakeLists.txt`: RunWorker.cpp added.
- `tests/server/ServerTestSupport.h`: test::WaitUntil and test::MakeWorker, verbatim.
- `tests/server/RunWorkerTests.cpp`: the brief's seven test cases, tagged [hosted-server][worker].

## Deviations from the brief

1. **Race fix in the worker loop (a real flaw).** The brief's loop exits only on `stop.stop_requested()`. RequestStop
   sets `_stopping` and stops the current `_maintenanceStop` under the mutex, and only afterwards calls
   `_thread.request_stop()`. If the worker thread takes the mutex in that gap with `_maintenanceDue` true and an empty
   queue (e.g. just after a job ended), it sees no thread stop, creates a fresh `_maintenanceStop` that nobody will
   ever stop, and runs Maintain to completion: RequestStop would not stop that fold, and the destructor would wait for
   a whole (possibly long, real-model) fold. Fix: the loop also returns when `_stopping` is set:
   `if (stop.stop_requested() || _stopping) return;` (with a comment saying why). Since `_stopping` is set in the same
   critical section that empties the queue and stops the current maintenance source, either the thread sees
   `_stopping` and exits, or its new maintenance source is created before RequestStop's critical section, which then
   stops it. No queued job can be lost (RequestStop already failed them all).
2. **Test expectation fixed: Reopen with unembeddable lore throws core::DataError, not inference::InferenceError.**
   HostedSession::Open documents "DataError if lore cannot be embedded", and MemoryIndex::SyncLore
   (src/turn/Retrieval.cpp:309-311) wraps the embedder's InferenceError into
   `DataError("host data: lore entry 'The road' cannot be embedded: ...")`. The brief's
   `CHECK_THROWS_AS(worker->Reopen(unembeddable, 42), inference::InferenceError)` failed with exactly that DataError.
   The test now checks `CHECK_THROWS_WITH(..., ContainsSubstring("lore entry 'The road' cannot be embedded"))` and
   `CHECK_THROWS_AS(..., core::DataError)`; the following `look()` still checks the run reopened as it was (which it
   does: the previous data's lore vectors are already stored, so the fallback open embeds nothing). Code was not
   changed for this; the RunWorker.h comment for Reopen says "the error is rethrown" without naming a type, so it
   stays correct. Later tasks mapping Reopen errors to statuses should expect 400 (DataError) for unembeddable lore.
3. Mechanical: the test file also includes `server/SlotPool.h` (it names `server::SlotPool::UPKEEP_SLOT`; the brief's
   include list missed it, a compile error C3083).

## Review of the rest of the brief's code (no change needed)

- Member order: `_thread` is declared last, so it is joined first on destruction, before `_session`, `_backend` and
  `_pModels`; `_session` goes before `_backend`/`_pModels`.
- Constructor failure: the thread sets the exception and returns; `openedResult.get()` throws; `_thread`'s destructor
  joins an already-finished thread. No session, no file (checked by the test).
- `_session` and `_backend.SetSlot` are only touched on the worker thread; `_queue`, `_maintenanceDue`,
  `_maintenanceStop` and `_stopping` only under `_mutex`. The maintenance token is copied under the lock, and Enqueue
  stops the current source under the same lock, so a job never waits behind an unstoppable fold.
- Reopen's `throw;` after the nested try/catch rethrows the outer (new data's) exception, as intended.
- Not changed, noted: a successful Reopen updates `_data` but not `_seed`. Harmless: the seed only matters when the
  file is missing, and after any successful open the file exists.

## TDD evidence

RED: `cmake --build --preset tests`
```
D:\MorpheusEngine\tests\server\RunWorkerTests.cpp(22,1): error C1083: Cannot open include file: 'server/RunWorker.h': No such file or directory
```
First GREEN attempt with the brief's test verbatim (after adding the SlotPool include):
```
D:\MorpheusEngine\tests\server\RunWorkerTests.cpp(267): FAILED:
  CHECK_THROWS_AS( worker->Reopen(unembeddable, 42), inference::InferenceError )
due to unexpected exception with message:
  host data: lore entry 'The road' cannot be embedded: text of 210 bytes is over the embedding limit
test cases:  18 |  17 passed | 1 failed
```
GREEN (after deviation 2), `cmake --build --preset tests` clean (no warnings), then six runs of
`./build/tests/bin/Debug/morpheus_tests.exe "[worker]" | tail -2` (the tag also matches 11 older [worker] tests):
```
All tests passed (202 assertions in 18 test cases)
All tests passed (201 assertions in 18 test cases)
All tests passed (201 assertions in 18 test cases)
All tests passed (201 assertions in 18 test cases)
All tests passed (201 assertions in 18 test cases)
All tests passed (202 assertions in 18 test cases)
```
(The assertion count varies by one because the fold test's slot loop counts the fold calls, which depends on how
often a polling Call stopped a fold.)
`./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server][worker]"`: All tests passed (34 assertions in 7 test cases).
Full suite: `ctest --preset tests`: 100% tests passed out of 534 (the [server]/[llama] tests skipped as usual).

ASan: `cmake --preset asan` (exit 0), `cmake --build --preset asan` (exit 0; only the usual LNK4300 /INCREMENTAL
note), then three runs of `./build/asan/bin/Debug/morpheus_tests.exe "[hosted-server][worker]"`: All tests passed
(34 assertions in 7 test cases) each time; and `"[worker]"`: All tests passed (203 assertions in 18 test cases).
No ASan reports.

## Encoding checks

`LC_ALL=C grep -c $'[\x80-\xFF]'` and `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'` on every changed file:
```
src/server/RunWorker.h 0 0
src/server/RunWorker.cpp 0 0
src/server/CMakeLists.txt 0 0
tests/server/ServerTestSupport.h 0 0
tests/server/RunWorkerTests.cpp 0 0
```
(No escape sequences in this task's files.)

## Files changed

- D:/MorpheusEngine/src/server/RunWorker.h (new)
- D:/MorpheusEngine/src/server/RunWorker.cpp (new)
- D:/MorpheusEngine/src/server/CMakeLists.txt
- D:/MorpheusEngine/tests/server/ServerTestSupport.h (new)
- D:/MorpheusEngine/tests/server/RunWorkerTests.cpp (new)

## Concerns

- The fold-stop test is timing-based (< 600 ms for a job behind a fold whose pieces take 100 ms). It passed in all 9
  runs (6 tests preset, 3 asan); a heavily loaded machine could still exceed it.
- A job that sets no slot inherits whatever slot the previous job or fold left (e.g. the upkeep slot). The brief makes
  slot choice the job's duty; later tasks' jobs must always call SetSlot before model calls.
- Reopen's unembeddable-lore error is DataError (see deviation 2): the Api task should map it accordingly.
- No morpheus_tests or ctest process left running (tasklist checked).
