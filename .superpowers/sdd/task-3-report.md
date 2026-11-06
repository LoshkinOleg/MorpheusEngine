# Task 3 report

Status: DONE

## Implemented
- TurnRecord::attempt; turns.attempt column (SCHEMA_VERSION stays 1); written by CommitTurn, read by Turns/Turn.
- EmbeddingKind::Note ("note"); TruncateAfter deletes turn and note embeddings after the turn.
- Embeddings() ordered by CAST(ref_id AS INTEGER), ref_id.
- RunStore::ReplaceSummaries (ContractError on a bad range, checked before the transaction; delete+insert in one `_db.Transaction` lambda, the store's existing spelling).

## TDD
- RED: `cmake --build --preset tests` failed: 'attempt' not a member of TurnRecord, 'ReplaceSummaries' not a member of RunStore, EmbeddingKind::Note undeclared.
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[store]"` -> All tests passed (126 assertions in 22 test cases). `ctest --preset tests` -> 100% tests passed out of 114.

## Files
src/core/TurnRecord.h, src/store/RunStore.h, src/store/RunStore.cpp, tests/store/RunStoreTests.cpp

## Deviations
- The brief's ReplaceSummaries snippet used `Transaction transaction(_db); ... Commit()`; the store has no such class, so I used `_db.Transaction([&]{...})` as CommitTurn does (allowed by the brief). The error message is formatted with the range, like AddSummary.
- Line endings: tests file was mixed LF/CRLF after my append; normalised to CRLF (git autocrlf is on).

## Concerns
None.
