# D2 Task 1 report: store version 4

Status: DONE. Commit 1a54def.

Implemented: HostedTurn::eventsOmitted; SCHEMA_VERSION 4; hosted_turns.events_omitted (fresh schema, EVENTS_OMITTED_SQL upgrade from 3, version 2 upgrades straight to 4); CommitHostedTurn require + insert; HostedTurns read with negative check. Four new tests, upgrade test renamed/updated.

TDD: the RED compile-error run was skipped (tests and implementation written before the first build). GREEN: `[store]` 41 cases pass, `[hosted]` 66 cases pass, `ctest --preset tests` 100% passed out of 510.

Files: src/core/HostedRecord.h, src/store/RunStore.h, src/store/RunStore.cpp, tests/store/HostedStoreTests.cpp, tests/store/RunStoreTests.cpp.

Deviations: tests/store/RunStoreTests.cpp also changed (SCHEMA_VERSION == 3 check and "not the supported version 3" message now 4), per step 5. RunStore.h comments updated to version 4.

Byte greps (non-ASCII / control bytes): all five files 0 / 0. No morpheus/ctest processes left. Git warns about LF->CRLF only.
