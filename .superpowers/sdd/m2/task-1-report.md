# Task 1 report: Close the M1 hardening leftovers

Status: DONE. Commit ae7fca5 "Close M1 hardening leftovers".

## Implemented
- World.cpp: after the CRLF-to-LF loop, a remaining '\r' throws DataError ("'<file>': stray carriage return at byte N"). The loop comment now says a lone CR is rejected. Added MORPHEUS_REQUIRE(pCheckType != nullptr, ...) before the stats loop (the first use of pCheckType).
- StateSchema.cpp FromJson: the field type is validated against {int, enum, string, list, counter} (same "unknown type '<name>' (expected ...)" wording) before the unknown-key check. The allowed key set is built after that. The old trailing `else` that threw is now the plain counter branch.
- JsonInt.h: doc comment extended with the ContractError sentence.
- Changes.cpp: the two remaining numeric_limits<int>::max() in the counter branch now use INT_MAX_VALUE. No INT_MIN occurrences were left.
- RunStore.h: Turn/Turns doc mention StoreError if a note has no matching turn row. RunStore.cpp: CommitTurn comment updated.
- Tests: lone-CR section; "itn" schema test; notes-rollback test (store); stat-not-int asserts "must be an int field" (actual message: "stat 'location' must be an int field, but it is enum", still names the stat); ReadLfText now converts only CRLF pairs and, with ConvertWorldToCrlf, is in the anonymous namespace; the MORPHEUS_REQUIRE location test moved to new tests/core/ErrorsTests.cpp with tag [errors] (asserts "ErrorsTests.cpp:"). TestPathsTests.cpp keeps its WriteFile test, so it was not deleted; its now-unused core/Errors.h include was dropped.

## TDD evidence
I implemented src before running RED, so I stashed the src changes (`git stash push -- src`), kept the new tests, then restored them.

RED: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[world],[schema],[store],[errors]"`
- "An unknown field type is reported before its keys" FAILED: message was "...field 'bad': unknown key 'max'".
- "LoadWorld reports the file at fault / lone carriage return" FAILED: no exception thrown.
- `test cases: 38 | 36 passed | 2 failed`. The rollback test passed on the existing code (kept as a regression guard).

GREEN: after `git stash pop`, `cmake --build --preset tests && ctest --preset tests`
- `100% tests passed out of 80`. `[errors]` ran separately: 1 test case passed. No build warnings or errors.

## Files changed
src/core/World.cpp, StateSchema.cpp, JsonInt.h, Changes.cpp; src/store/RunStore.h, RunStore.cpp; tests/core/WorldTests.cpp, StateSchemaTests.cpp, ErrorsTests.cpp (new); tests/store/RunStoreTests.cpp; tests/TestPathsTests.cpp.

## Deviations
- Only the TDD order (src first, then RED via stash), as above.
- The StateSchema type check uses a `knownTypes` set, then the existing if/else chain with the final branch as counter, instead of repeating the unknown-type throw.
- Notes-rollback test placed before "TruncateAfter..." (after the existing failed-CommitTurn notes test), since it needs the TestTurn/StateWithHealth helpers defined there.

## Concerns
- Working-tree line endings are mixed (autocrlf on: src files CRLF in the working copy, some test files LF); the index is LF. Git warns on commit but the result is normal.
