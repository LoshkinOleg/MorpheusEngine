# Task 2 report: turn-record types move into core

Status: DONE. Commit 256b4e6 "Move typed turn records into core".

## Implemented
- `src/core/TurnRecord.h/.cpp`: CheckOutcome, TurnRecord, Summary, Note and the CheckOutcome/AppliedChange/RejectedChange ToJson/FromJson helpers (DataError on bad shape), verbatim from the brief. Added `TurnRecord.cpp` to `src/core/CMakeLists.txt` after `JsonInt.cpp`.
- `src/store/RunStore.h`: removed TurnRecord/Summary/Note, includes `core/TurnRecord.h`, re-exports with `using core::...`.
- `src/store/RunStore.cpp`: column `rolls` renamed `checks`; CommitTurn serializes typed members; Turns selects `checks` and defines `convertColumn` right after `parseColumn` in the row loop.
- Tests: new `tests/core/TurnRecordTests.cpp`; `tests/store/RunStoreTests.cpp` TestTurn body replaced, assertions migrated, new "wrong shape" test added. No other tests read the old JSON members.

## TDD evidence
RED: `cmake --build --preset tests` -> `TurnRecordTests.cpp(5,1): error C1083: Cannot open include file: 'core/TurnRecord.h'`.
GREEN: `cmake --build --preset tests && ctest --preset tests` -> `100% tests passed out of 84`. Focused `"[turnrecord],[store]"` -> `All tests passed (135 assertions in 22 test cases)`.

## Files changed
src/core/CMakeLists.txt, src/core/TurnRecord.h, src/core/TurnRecord.cpp, src/store/RunStore.h, src/store/RunStore.cpp, tests/core/TurnRecordTests.cpp, tests/store/RunStoreTests.cpp

## Deviations
None. The only adaptation: `TestTurn` already had the `notes` parameter, so I kept its signature and set `input` before `notes` as in the brief body.

## Concerns
None. Git prints LF->CRLF warnings on the new files (autocrlf); not a content issue. Only the `tests` preset was built.
