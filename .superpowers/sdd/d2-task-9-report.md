# D2 Task 9 report: the turn route

Status: DONE_WITH_CONCERNS (concerns are minor; see the end)

Commit: 63606d1 Api: the turn route streams its action, tokens, sentences and done, on its conversation's slot

## What I implemented

- `tests/server/ApiTurnTests.cpp`: the brief's five test cases ([hosted-server][api]), verbatim, plus `#include <optional>` (the cancel test uses `std::optional` directly).
- `src/server/Api.cpp`:
  - includes `<chrono>`, `<future>`, `<memory>`, `"server/SentenceSplitter.h"`;
  - a `// ---- Constants` group first in the anonymous namespace with `START_POLL` (100 ms);
  - `POST /v1/runs/{run_id}/conversations/{cid}/turn` before the `events` branch: the request's shape checked on the handler thread (`speaker`, `"decide"` refused with 400, `player_text`, `events`, `observation`, `allowed_actions`), the turn posted as a job on the run's worker (conversation check, `SlotPool::Acquire` -> `SlotBackend::SetSlot`, `RunTurn` with onAction/onToken pushing `action`/`token`/`sentence`, then the last `sentence` from `Finish` and `done`, or `error`), the stream cancelled with the worker's stop token, and the handler waiting for the first event: an `error` first is the plain JSON answer with its status, anything else returns `ApiResponse::stream`; a turn the worker refused (stopping) rethrows its future's InferenceError (503).

## Deviation from the brief (a contract gap fixed)

The brief logged a 500 only in the handler, when the error was the stream's first event. An error after the action (e.g. a ContractError or other bug during the speak call, or the commit) went out on the stream as `error` with status 500, but nothing logged it, while `Api::Handle`'s contract says "a 500 is logged too". The handler cannot log it either: it may have returned the stream long before.

Fix: the job captures a copy of `_log` and the route's name (`std::format("{} {}", method, path)`, a std::string so it cannot dangle after the handler returns) and logs any 500 in its own catch block, before or after the action, in the same "<METHOD> <path> failed: <message>" form. The handler no longer logs the first-event 500 (it would be logged twice). The route's contract (statuses, bodies, events) is unchanged.

Everything else in the brief's code I checked and kept: the job captures the stream by value (a shared_ptr), the request by move, the slot key by value and the pool's pointer; the `stop_callback` captures the lambda's own `stream` member, which lives for the whole job; the callbacks capture by reference only locals that outlive `RunTurn`; Catch2 macros stay on the test thread. The wait loop cannot spin on an invalid future: the job always calls `End`, so the future is ready only after the stream started, except for a refused job, whose `get()` throws.

## TDD evidence

RED: `cmake --build --preset tests` (clean), then each new case by name:
```
== A turn streams*                 test cases: 1 | 1 failed   exit=42
== A refused action streams*       test cases: 1 | 1 failed   exit=42
== A turn's errors before*         "there is no route POST /v1/runs/save_1/conversations/c1/turn" contains: ...   exit=42
== A turn's calls use*             test cases: 1 | 1 failed   exit=139 (segfault)
== A turn whose stream*            test cases: 1 | 1 failed   exit=42
```
Every turn request answered 404 "there is no route POST ...". The slots case crashes in RED because the brief's test dereferences the (null) stream of a 404 answer (`test::ReadAll(*harness.Post(...).stream)`); running "[api]" as a whole therefore also segfaults in RED. That is the test as written; it passes once the route exists.

GREEN:
```
./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"
All tests passed (780 assertions in 39 test cases)
./build/tests/bin/Debug/morpheus_tests.exe "[api]"   (3 runs)
All tests passed (235 assertions in 10 test cases)   x3
ctest --preset tests
100% tests passed out of 553 (the 3 real llama-server tests skipped, as without MORPHEUS_TEST_SERVER_URL)
cmake --build --preset asan; ./build/asan/bin/Debug/morpheus_tests.exe "[hosted-server]"
All tests passed (780 assertions in 39 test cases)
```
Builds were clean (no compiler warnings); the asan link prints its usual LNK4300 note about /INCREMENTAL.

## ASCII checks

```
src/server/Api.cpp: [\x80-\xFF] 0, control bytes 0
tests/server/ApiTurnTests.cpp: [\x80-\xFF] 0, control bytes 0
```
No escape sequences were written in this task. No morpheus_tests or ctest process left running.

## Files changed

- D:/MorpheusEngine/src/server/Api.cpp
- D:/MorpheusEngine/tests/server/ApiTurnTests.cpp (new)

## Concerns

1. A turn posted for a conversation that has ended (but exists) passes `RequireConversation`, acquires a slot, and only then fails in `RunTurn` with the 400 DataError. The ended conversation then holds a conversation slot until LRU eviction gives it to another conversation. Harmless (the slot is reclaimed when needed, and `end` already released it once), but a later task could check `ended` before `Acquire`. I left it as the brief wrote it.
2. The handler blocks until the turn's first event (the resolve call), with no timeout of its own; that is by design (the worker's stop token ends it on shutdown), noted for the HttpServer task.
