# E2 Task 4 report: Rewind, abandoning on new records, the reload fixture

Status: DONE

## Commits

- 9ab74b1 Rollback store: the writers of new records refuse while a rollback is open; tests of an undone condense's
  links and embedding, and a conversation started after the rollback (additions A and B)
- d960528 Rewind: a hosted run as it was at a game time; a new record abandons the hidden future; R6's reload fixture

## What I implemented

### Store (addition A)
- `RunStore::RequireNoOpenRollback(const char* method)`, a private helper (six callers): `MORPHEUS_REQUIRE` that no
  rollback is open, message "RunStore::<method>: a rollback is open; abandon it before storing a new record".
- Called right after `RequireHosted` in `CreateConversation`, `SetParticipants`, `EndConversation`,
  `AddConversationEvents`, `CommitHostedTurn` and `AddPendingShare`. Their header comments name the new ContractError.
  Summary writes, `MarkFolded`, `RememberFold`, `SetRememberedThrough` and `MarkShareApplied` are untouched.

### Store tests (additions A and B), `tests/store/RollbackStoreTests.cpp`
- "The writers of new records refuse while a rollback is open": each of the six throws ContractError naming its
  method; the undo then restores the run unchanged.
- "An undo deletes a condense made while the rollback was open, with its links and its embedding" (beside the existing
  condense test): condense npc-1, roll back to 2.7, condense again (links the 1-2 shown again), put a summary embedding
  on the refold; after the undo the embedding is gone, npc-1 shows only the first condense, every other npc-1 summary
  has supersededBy == the first condense, `RememberedThrough(id, "npc-1") == 4`, and (read directly from
  summary_supersessions) the refold has 0 links while the first condense keeps its 2.
- "A conversation started after the rollback's time is hidden whole and comes back whole on undo": c2 started at 5.0
  with turn 5 and an event at 5.5, plus a c1 event at 5.5; rollback to 4.7 hides c2 whole (`Conversations()` leaves it
  out, `hiddenTurns == 1`, no turns or events) and the c1 event; the undo restores both (`restoredTurns == 1`).

### Session
- `turn::RewindResult` and `HostedSession::Rewind(double)` verbatim from the brief (with the comment); `SyncHistory()`
  after it.
- Private `AbandonRollbackForNewRecord()`, called after the checks and before the first write in `StartConversation`,
  `RecordEvents`, `SetParticipants`, `EndConversation`, `RunTurn` (after the narrate-allowance check, before the turn
  number, the reads and the catch-up) and `ShareMemories` (after the player check). `Maintain` never calls it. A class
  comment above `StartConversation` lists the writers.

### Fixture runner and fixture
- `tests/hosted/fixtures/reload.json` verbatim from the brief.
- `PlayMemoryFixture` (four-argument form): the fixture's `clock` sets the host data's clock; a local lambda
  `observationOf(step)` writes a step's `clock` at the pointer for turn, leave, join and start steps. New steps
  `rewind`, `turns`, `conversation`, `events` as in the brief. The doc comment lists them among what is checked with
  `echoedMemories` false.
- `tests/hosted/HostedRewindTests.cpp` verbatim; the fixture test case added to `HostedFixtureTests.cpp`.
- `tests/hosted/fixtures/README.md`: reload.json in Files; the memory fixture format documents `clock` (top level and
  per step) and the four new steps.

### Addition C
The fixture's expectations held on the first run; nothing to trace or change. I walked it through beforehand: fold
summaries carry no clock (only shared copies do), so the rewind to 2.5 hides Hulda's 1-3 fold by its range (mark back
to 0) and Saadia's copy by its clock 4.0; Saadia's own 1-2 fold stays visible (mark min(3, 2) = 2). The second
maintain refolds turns 1-2 for Hulda only; the undo deletes that refold.

## TDD evidence

Store (A), RED: `./build/tests/bin/Debug/morpheus_tests.exe "[rollback]"`:
```
D:\MorpheusEngine\tests\store\RollbackStoreTests.cpp(275): FAILED:
  CHECK_THROWS_MATCHES( store.EndConversation(id, 2.8), core::ContractError, ... )
because no exception was thrown where one was expected:
... (same for CreateConversation, SetParticipants, AddConversationEvents, CommitHostedTurn, AddPendingShare)
  CHECK( store.Conversations().size() == 1 )  with expansion: 2 == 1
test cases:  26 |  25 passed |  1 failed
assertions: 226 | 216 passed | 10 failed
```
The two B tests passed on first run: they cover undo statements Task 3 already had, which no test exercised.

Store GREEN: `[rollback]` All tests passed (226 assertions in 26 test cases); `[store]` 674 assertions in 79 test
cases; `[hosted]` 1207 assertions in 117 test cases; `ctest --preset tests` 100% tests passed out of 640, run with the
session tests moved aside so the store commit was checked alone.

Session RED: `cmake --build --preset tests`:
```
tests\hosted\HostedTestSupport.h(316,55): error C2039: 'Rewind': is not a member of 'morpheus::turn::HostedSession'
tests\hosted\HostedRewindTests.cpp(24,9): error C2039: 'Rewind': is not a member of 'morpheus::turn::HostedSession'
```
Session GREEN (build clean, no warnings):
```
[rewind]: All tests passed (68 assertions in 3 test cases)
[memory]: test cases: 35 | 34 passed | 1 skipped; assertions: 357 | 357 passed   (the skip is a [llama] case)
[hosted]: All tests passed (1275 assertions in 120 test cases)
[hosted-server]: All tests passed (1157 assertions in 58 test cases)
[turn]: All tests passed (163 assertions in 16 test cases)
[replay]: All tests passed (501 assertions in 7 test cases)
[rollback]: All tests passed (226 assertions in 26 test cases)
ctest --preset tests: 100% tests passed out of 643
```

## Byte checks (non-ASCII count, control-byte count)
```
src/store/RunStore.h 0 0
src/store/RunStore.cpp 0 0
tests/store/RollbackStoreTests.cpp 0 0
src/turn/HostedSession.h 0 0
src/turn/HostedSession.cpp 0 0
tests/hosted/HostedTestSupport.h 0 0
tests/hosted/HostedRewindTests.cpp 0 0
tests/hosted/HostedFixtureTests.cpp 0 0
tests/hosted/fixtures/reload.json 0 0
tests/hosted/fixtures/README.md 0 0
```
reload.json's escaped quotes (`\"action\"`) are still literal backslash sequences (4 matches). I wrote it through a
quoted heredoc.

## Files changed
- src/store/RunStore.h, src/store/RunStore.cpp, tests/store/RollbackStoreTests.cpp (commit 1)
- src/turn/HostedSession.h, src/turn/HostedSession.cpp, tests/hosted/HostedRewindTests.cpp (new),
  tests/hosted/fixtures/reload.json (new), tests/hosted/fixtures/README.md, tests/hosted/HostedTestSupport.h,
  tests/hosted/HostedFixtureTests.cpp (commit 2)

## Deviations from the brief
- The brief says `open.json`'s host data gets the clock. I left `open.json` unchanged: the runner sets
  `HostData::clock` from the fixture's top-level `clock` on `FixtureHostData()`, as the brief's Step 2 says.
- The store test of the six writers uses `CHECK_THROWS_MATCHES` with `MessageMatches`, which needs
  `catch_matchers_exception.hpp` in RollbackStoreTests.cpp.
- The `rewind`, `turns`, `conversation` and `events` steps carry short comments, and `HostedTestSupport.h` gains the
  `<optional>` and `core/HostedRecord.h` includes.
- In `ShareMemories`, the abandon comes after all checks, before the complete/pending branch. So a share already
  stored as pending, which writes nothing, still abandons an open rollback. This follows the brief's "once it passed
  its checks".

## Concerns
- `ctest --preset asan` was not run (the instructions name only the tests preset).
- The HTTP memory-fixture runner (`tests/server/ServerTestSupport.h`, `PlayMemoryFixtureOverHttp`) does not know the
  new steps and does not run reload.json. I assume the API task (rewind route, 1.2.0) adds them.
