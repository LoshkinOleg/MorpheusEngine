# Task 9 report: turns, snapshots and undo

Status: DONE. Commit 67e86dd "Add turn commits, snapshots and undo to RunStore".

## Implemented
- `TurnRecord` struct, `RunStore::CommitTurn` (checks number == LatestTurn()+1 via MORPHEUS_REQUIRE, turn + snapshot in one transaction), `Turn`, `Turns`, `TruncateAfter` (turns, snapshots, summaries ending after, notes, turn embeddings; one transaction).
- Controller decision applied: in `Turns`, the five JSON columns go through a local `parseColumn` lambda that converts `nlohmann::json::parse_error` into `StoreError` naming file, turn number and column.
- Added test "RunStore::Turn rejects an unparseable stored column" (corrupts `resolve` via raw Database; checks StoreError and message contains `resolve`, `turn 1`, `run.db`).

## TDD evidence
RED: `cmake --build --preset tests` failed: `RunStoreTests.cpp(202,20): error C4430: missing type specifier` (TurnRecord unknown) and `error C2039: 'CommitTurn': is not a member of 'morpheus::store::RunStore'`.
GREEN: `cmake --build --preset tests` OK; `./build/tests/bin/Debug/morpheus_tests.exe "[store]"` -> `All tests passed (71 assertions in 13 test cases)`. `ctest --preset tests` -> `100% tests passed out of 54`.

## Files changed
src/store/RunStore.h, src/store/RunStore.cpp, tests/store/RunStoreTests.cpp (added `#include <format>`).

## Deviations
- Only the extra corrupt-column test and parseColumn lambda (per controller). Header doc comments for Turn/Turns also mention StoreError.
- Commit trailer uses Claude Sonnet 5.5 per instructions (brief said Opus 5.5).

## Concerns
- Test coverage for the corrupt-column path checks only `resolve`; the other four columns share the lambda.
- Git warns LF will become CRLF in tests/store/RunStoreTests.cpp (autocrlf, harmless).
