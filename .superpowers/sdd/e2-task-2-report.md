# E2 Task 2 report: store version 6

Status: DONE. Commit 5dc78cb "Store version 6: clocks on hosted records, conversations as history, superseded summaries name their condense, visible-only readers".

## Resolution (coordinator's decision: option 1)

The first pass stopped with NEEDS_CONTEXT, because a share applied with no clock stayed pending (described below). The coordinator chose option 1, now implemented:
- `pending_shares` gains `applied INTEGER NOT NULL DEFAULT 0`. It is the last line of `V6_COLUMNS_SQL`, so new files and upgrades both get it, and the test helper `DROP_VERSION_6_SQL` drops it again.
- `MarkShareApplied(id, clock)` sets `applied = 1, applied_clock = clock, unapplied_by = 0`.
- `PendingShares()` reads `hidden_by = 0 AND (applied = 0 OR unapplied_by <> 0)`.
- `LatestClock()` is unchanged: `MAX` ignores a NULL applied clock.
- `core::PendingShare` gets no `applied` field. No reader needs it, so it lives in SQL only.

New test: "A share applied in a run with no clock is not pending afterwards, and its row stays" (store level). It checks:
- the share is not pending after `MarkShareApplied(id, nullopt)`;
- `LatestClock()` is still nullopt;
- the share is still not pending after a reopen;
- the row is kept with `applied = 1` and a NULL `applied_clock`.

The session-level case (applied once, not pending afterwards) is the existing HostedMemoryTests "A share made while the conversation is open waits, then applies once it ended...". It now passes.

- RED for the new test, after its build: 3 failed assertions. Two `CHECK(PendingShares().empty())` failed, and the SQL threw "no such column: applied".
- GREEN, after the column (`cmake --build --preset tests`: no errors, no warnings):
  - `[store]` all passed (62 cases, 494 assertions);
  - `[hosted]` all passed (112 cases, 1169 assertions);
  - `[memory]` 357/357 assertions passed;
  - `[hosted-server]` all passed (58 cases);
  - `[turn]` all passed (16 cases);
  - `[replay]` all passed (7 cases);
  - `[context]` all passed (29 cases).
- `ctest --preset tests`: exit code 0, 623 tests. Five were skipped: the env-gated real llama-server and morpheus_server tests (611, 612, 613, 615, 616).
- Byte checks after the last edits: RunStore.cpp, RunStore.h and HostedStoreTests.cpp each 0 and 0. No morpheus_tests or ctest process is left running.

## The original blocker (now resolved): a share applied with no clock stayed pending

The brief says:
- `pending_shares.applied_clock` is NULL while a share is pending.
- `PendingShares()` filters on `applied_clock IS NULL OR unapplied_by <> 0`.
- `Maintain` calls `MarkShareApplied(share.id, _store.LatestClock())`.

In a run whose host data names no clock, `LatestClock()` is nullopt. That covers every existing test fixture and every host that has not adopted the clock. `MarkShareApplied(id, nullopt)` then writes NULL, so the applied share is still pending:
- `PendingShares()` keeps returning it.
- Every `Maintain` pass applies it again; that copies nothing, since its copies already exist.
- `ShareMemories` still treats it as stored.

The format has no way to say "applied, with no clock". The brief's own requirement ("No behaviour changes otherwise: every hosted test passes as before") fails here:

```
./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"
A share made while the conversation is open waits, then applies once it ended  HostedMemoryTests.cpp(289): CHECK(PendingShares().empty()) FAILED
Maintain asked to yield applies one pending share per pass; the next pass ...   HostedMemoryTests.cpp(632), (633) FAILED
A hosted run keeps pending shares until they are applied                        HostedStoreTests.cpp(971) FAILED (MarkShareApplied(share, std::nullopt))
test cases: 111 | 108 passed | 3 failed
```

Options (I did not pick one):
1. **Add a flag column (my recommendation).** Add `applied INTEGER NOT NULL DEFAULT 0` to `pending_shares`.
   - Pending becomes `applied = 0 OR unapplied_by <> 0`.
   - `MarkShareApplied` sets `applied = 1` and stores the clock, which may be NULL.
   - Task 3: abandoning resets `applied = 0, applied_clock = NULL`. Unapplying still compares `applied_clock > T`, so a NULL clock is never unapplied, which matches "a null clock is never hidden".
   - This changes the spec's section 4 columns.
2. **Delete the row instead.** When the clock is nullopt, `MarkShareApplied` deletes the row, as version 5 did. A share applied with no clock can never be unapplied, so keeping its row gains nothing. This contradicts "applied shares stay as rows".
3. **Something else** the spec owner prefers.

Whichever is chosen, only `RunStore::MarkShareApplied` / `PendingShares` (and the schema text, if option 1) and the expectation in `tests/store/HostedStoreTests.cpp` "A hosted run keeps pending shares until they are applied" change. Everything else below stands.

## What was implemented

- **Records** (`src/core/HostedRecord.h`, `src/core/TurnRecord.h`): the fields listed under Produces, each placed last and defaulted:
  - `HostedTurn::clock`, `Conversation::clock`, `ConversationEvent::clock`;
  - `PendingShare::clock`, `appliedClock`, `unappliedBy`;
  - `Summary::supersededBy`, `madeIn`, `clock`. `Summary::superseded` stays, now computed by the readers.
- **Sqlite** (`src/store/Sqlite.h/.cpp`): `Bind(int, double)`, `Bind(int, std::optional<double>)` (NULL for nullopt), `ColumnDouble`, `ColumnIsNull`, plus `Bind(int, int)` (see deviations).
- **RunStore** (`src/store/RunStore.h/.cpp`):
  - `SCHEMA_VERSION = 6`.
  - Three version 6 constants: `V6_COLUMNS_SQL`, `V6_TABLES_SQL` (`conversation_changes`, its index, `rollbacks`) and `V6_DROPPED_COLUMNS_SQL` (`summaries.superseded`, `conversations.participants`/`ended`). New files and upgrades both use them, so both end with the same tables and columns.
  - The `upgradeTo6` lambda (the brief's code). It runs after the version 2-4 steps, or alone for version 5, in the same transaction.
  - Writers take clocks:
    - `CreateConversation` writes the start row and the first change row in one transaction.
    - `SetParticipants` and `EndConversation` append change rows.
    - `AddConversationEvents`, `CommitHostedTurn` and `AddPendingShare` store the clock.
    - `AddActorSummary` stores `clock`, `superseded_by` and `made_in`, the last read with `OpenRollbackId()`.
    - `CondenseActorSummaries` inserts the condensed summary first, then sets `superseded_by` on the shown summaries.
  - `MarkShareApplied` replaces `RemovePendingShare`; `NextHostedTurn()` and `LatestClock()` (the brief's SQL) are new.
  - Every hosted reader skips hidden rows. `Conversation`/`Conversations` take participants and end from the latest visible change row; a conversation with none is a StoreError naming the file and `conversation_changes`.
  - `ActorSummaries`/`HostedSummaries` compute `superseded` with the brief's EXISTS SQL.
  - `LatestHostedTurn` counts visible turns only; `CommitHostedTurn` requires `NextHostedTurn()`.
- **HostedSession** (`src/turn/HostedSession.cpp`):
  - `StartConversation` and `SetParticipants` pass `observed.clock`; `RunTurn` sets `record.clock = observation.clock` and numbers the turn `NextHostedTurn()`.
  - `RecordEvents`, `EndConversation`, `ApplyShare`'s copies (read once before the loop), the pending row in `ShareMemories` and `Maintain`'s `MarkShareApplied` all use `_store.LatestClock()`.

## Tests

- `tests/store/HostedStoreTests.cpp`:
  - the brief's five tests and the version 5 upgrade test;
  - `V5_SCHEMA_SQL`, copied from `git show HEAD:src/store/RunStore.cpp`, and the `V5HostedFile` helper;
  - every writer call passes `std::nullopt`, and the version checks expect 6;
  - the damaged-participants sections now damage `conversation_changes`.
- Added beyond the brief's list, each covering new behaviour:
  - "A version 5 superseded summary with no later condense fails the upgrade" (the upgrade's StoreError);
  - a section where a conversation with no change row is a StoreError;
  - "A new file and an upgraded one have the same tables, indexes and columns": `PRAGMA table_info` plus `sqlite_master`, for hosted (new vs v5 upgraded) and owned (new vs v2 upgraded). It passes.
- `tests/store/RunStoreTests.cpp`: version 6, and "Statement binds and reads doubles and NULLs".

RED (`cmake --build --preset tests`, before the implementation): 54 compile errors. Examples:
- `HostedStoreTests.cpp(230,19): error C2660: 'RunStore::CreateConversation': function does not take 2 arguments`
- `HostedStoreTests.cpp(970,11): error C2039: 'MarkShareApplied': is not a member of 'RunStore'`
- `HostedStoreTests.cpp(1181,5): error C2039: 'LatestClock': is not a member`
- `HostedStoreTests.cpp(1184,10): error C2039: 'clock': is not a member of 'HostedTurn'`
- `RunStoreTests.cpp(202,5): error C2039: 'ColumnIsNull': is not a member of 'Statement'`

After the implementation (`cmake --build --preset tests`: no errors, no warnings):
- `[store]`: 61 test cases, 60 passed, 1 failed (the share above).
- `[hosted]`: 111 test cases, 108 passed, 3 failed (the share issue; listed above).
- `[memory]`: 4 failed assertions, in the same failing test cases.
- `[hosted-server]`: all passed (58 cases).
- `[turn]` 16, `[replay]` 7, `[context]` 29: all passed.
- `ctest --preset tests`: not run at this point; it passed after the fix (see Resolution).

Byte checks (non-ASCII, control bytes) on every changed file: 0 and 0 for each of:
- src/core/HostedRecord.h, src/core/TurnRecord.h
- src/store/Sqlite.h, src/store/Sqlite.cpp, src/store/RunStore.h, src/store/RunStore.cpp
- src/turn/HostedSession.cpp
- tests/store/HostedStoreTests.cpp, tests/store/RunStoreTests.cpp

No test or build process is left running.

## Deviations (mechanical)

- **`Statement::Bind(int, int)` added.** With `Bind(int, double)` present, an `int` argument converts to `std::int64_t` and to `double` equally well, so the call would be ambiguous. The new overload forwards to the int64 one.
- **New files reuse the version 5 schema texts.** They run `SCHEMA_SQL`, `HOSTED_SCHEMA_SQL` and `PER_ACTOR_TABLES_SQL` unchanged, then the same `V6_*` SQL as the upgrade. This keeps new and upgraded files identical without a second copy of every hosted table.
- **`V6_DROPPED_COLUMNS_SQL` is a constant.** It replaces the brief's inline `ALTER ... DROP COLUMN`, because both creation and the upgrade need it.
- **`DROP_VERSION_6_SQL` test helper.** The existing version 3 and 4 tests make old files from new ones. A new file is now version 6, so this helper first takes it back to version 5: participants and ended come back from the latest change row, and `superseded` from `superseded_by`. The tests then drop version 5 as before.
- **Renamed test titles:**
  - "upgraded ... to version 5" became "version 6", or "through version 5" for the version 4 hosted test.
  - "keeps pending shares until they are removed" became "until they are applied".
- **`OpenRollbackId()` has a single caller (`AddActorSummary`).** The brief names it; Task 3 adds more callers. When more than one rollback is open it throws StoreError rather than picking one (fail loud).
- **`AddActorSummary` stores `summary.supersededBy`**, as it used to store `superseded` ("every field but id"). `made_in` always comes from the open rollback, never from `summary.madeIn`.
- **The "placeholder body of the earlier version 5 test case" does not exist** at HEAD, so there was nothing to remove.

## Concerns

1. The blocker above is resolved by the `applied` column. The coordinator is updating the spec and Task 3's plan text (rollback apply, undo and abandon) for it. As it stands, abandon must reset `applied = 0` along with `applied_clock = NULL`.
2. Owned runs also drop `summaries.superseded` and the conversation columns during the upgrade. Their hosted tables are empty and no owned summary is superseded, so owned data is unchanged.

## Fix round (review e2-task-2-review.md): commit 96dc2c0

"Store version 6 review fixes: summary supersessions as links, a direct Conversation read, tests of hidden rows, made_in and open rollbacks", on top of 5dc78cb. Only src/core/TurnRecord.h, src/store/RunStore.h/.cpp and tests/store/HostedStoreTests.cpp are in it. Nothing under docs/ was touched.

### Changes

1. **Important: supersessions are links (the coordinator's decision).**
   - Schema:
     - `summaries.superseded_by` is gone from `V6_COLUMNS_SQL`.
     - `V6_TABLES_SQL` creates `summary_supersessions (summary_id, condensed_id, PRIMARY KEY (summary_id, condensed_id))`.
   - Condensing: `CondenseActorSummaries` inserts one link per shown summary it replaces, with `INSERT ... SELECT`. It never updates or deletes a link.
   - Readers: `SUMMARY_COLUMNS` computes both values from the links:
     - `superseded`: EXISTS a link to a visible summary;
     - `supersededBy`: `COALESCE(MIN(id) of the visible linked condenses, 0)`.
     - `core::Summary` keeps both fields, read and not stored.
   - Upgrade: version 5 to 6 inserts a link from each superseded summary to its condense, found as before. A missing condense is still a StoreError.
   - Tests: `DROP_VERSION_6_SQL` rebuilds `superseded` from the link table and drops it.
   - New test: "A summary keeps every condense that superseded it: undoing a later condense leaves the earlier one in force". It is the reviewer's exact case:
     - S and S2 are condensed into C1.
     - An open rollback (raw rows) hides C1 and S2; S is shown.
     - C2 condenses S, with made_in = 1.
     - The undo is simulated by hand: C2's links and C2 are deleted, C1 and S2 are unhidden.
     - Checks: S and S2 are superseded with supersededBy = C1, and only C1 is shown.
2. **Minor 1: visible-only tests.**
   - "Every hosted reader skips hidden rows", three sections:
     - a hidden conversation: `Conversation` and `Conversations` skip it, and `CommitHostedTurn` and `EndConversation` refuse it;
     - hidden change rows fall back to the earlier visible one: two participants, not ended;
     - hidden events are not read.
   - "Hidden shares are not pending; a share a rollback unapplied is pending again".
   - "The latest clock skips hidden rows and the applied clocks of unapplied shares": 4.0, then 2.5 once unapplied, then 1.0 once the share and the turn are hidden.
3. **Minor 2: made_in and open rollbacks.** New test "A summary written while a rollback is open is made in it; two open rollbacks are damaged saved data":
   - With no rollback, `madeIn` is 0. An undone rollback 2 does not count; with open rollback 3, `madeIn` is 3.
   - A second open rollback makes `AddActorSummary` throw a StoreError naming run.db and rollbacks, and nothing is written.
4. **Minor 3: comments over 120 columns.** Rewrapped:
   - RunStore.h: the CondenseActorSummaries comment, plus two more lines of mine at 121 and 122 columns (the AddActorSummary and HostedTurns comments);
   - HostedStoreTests.cpp: the DROP_VERSION_6_SQL comment.
5. **Minor 4: AddActorSummary's comment.** It now lists what is stored (fromTurn, toTurn, text, actorId, conversationId, toldBy, sourceId, clock) and what is ignored (id, superseded, supersededBy, madeIn). It no longer binds any supersession.
6. **Minor 5: a direct `Conversation(id)` read.** It reads its own start row (`WHERE id = ?1 AND hidden_by = 0`) and that row's latest visible change row.
   - The read is a new private helper, `ReadConversation(Statement& startRow)`, with two callers: `Conversation` and `Conversations`.
   - The helper selects its columns through a new constant, `CONVERSATION_COLUMNS`.

### TDD evidence

- RED: `cmake --build --preset tests` (clean), then `./build/tests/bin/Debug/morpheus_tests.exe "[rollback]"`:
  ```
  A summary keeps every condense that superseded it: undoing a later condense
  due to unexpected exception with message:
    SQL failed: no such table: summary_supersessions | DELETE FROM
  test cases: 14 | 13 passed | 1 failed
  ```
  The new Minor 1 and 2 tests passed at RED. They cover behaviour 5dc78cb already had, which is what the review asked for.
- GREEN: `cmake --build --preset tests` (no errors, no warnings), then:
  ```
  [rollback]: All tests passed (85 assertions in 14 test cases)
  [store]: All tests passed (533 assertions in 67 test cases)
  [hosted]: All tests passed (1208 assertions in 117 test cases)
  [memory]: assertions: 357 | 357 passed
  [hosted-server]: All tests passed (1157 assertions in 58 test cases)
  [turn]: All tests passed (163 assertions in 16 test cases)
  [replay]: All tests passed (495 assertions in 7 test cases)
  [context]: All tests passed (236 assertions in 29 test cases)
  ```
- `ctest --preset tests`: "100% tests passed out of 628". Five were skipped: the env-gated llama-server and morpheus_server real-server tests (616, 617, 618, 620, 621).
- Byte checks on TurnRecord.h, RunStore.h, RunStore.cpp and HostedStoreTests.cpp: 0 and 0 each.
- No morpheus_tests or ctest process is left running.

### Notes for Task 3

- Undo must delete the `summary_supersessions` rows whose `condensed_id` is a summary it deletes (`made_in` it). The test above does that by hand.
- Abandon must reset `applied = 0` with `applied_clock = NULL`.
