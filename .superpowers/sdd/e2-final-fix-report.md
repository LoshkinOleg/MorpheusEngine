# E2 final fix round: report

Start: 9292990. Five commits, one per item of `e2-final-fix-brief.md`, tests first.

| # | Commit | Subject |
|---|---|---|
| 1 | 69b927b | Rollback: hide every condense that holds a summary the rollback hid |
| 2 | 4d8a113 | Rewind: abandon the hidden future only when something is recorded |
| 3 | af4682d | Rollback store: an undo drops its saved marks, as an abandon does |
| 4 | a9607c5 | Host data: an empty clock is a DataError at /clock |
| 5 | 38d6432 | Rollback: a hidden-row test of HostedSummaries; docs |

## 1. A rollback hides every condense that holds what it hid (I1)

- `RunStore::ApplyRollback` (src/store/RunStore.cpp): after the existing summary hides and before the marks move, a
  loop runs `UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND id IN (SELECT l.condensed_id FROM
  summary_supersessions l JOIN summaries s ON s.id = l.summary_id WHERE s.hidden_by = ?1)` and stops when
  `SELECT changes()` reads 0 (a condense of a condense is hidden on the next pass). The mark rules are unchanged:
  they read only conversation-set, non-copy folds, so a hidden condense never moves a mark.
- Tests (tests/store/RollbackStoreTests.cpp):
  - "A rollback hides a condense that holds a shared copy made after its time" (review Scenario A: c1 1-3, c2 4-5,
    c3 6-7; npc-2's c2 memory shared to npc-1 at clock 7; npc-1 condenses [1-3, copy] into 1-5; rollback to 6.5 shows
    only the 1-3 fold; undo shows the condense again, 3 summaries in all).
  - "A rollback hides a condense whose range ends before the end of a hidden fold it holds" (Scenario B: c1 turns
    1,2,5,6, c2 3,4; npc-1 fold 1-6, copy 3-4 at 4, condense 1-4; rollback to 4 shows only the copy, npc-1's c1 mark
    0; undo shows the condense, mark 6).
- Docs: the spec's section 3 hidden-summary bullet (only that bullet); the `ApplyRollback` comment in RunStore.h.

RED: `morpheus_tests.exe "[rollback]"`: `test cases: 28 | 26 passed | 2 failed`, `CHECK( shown[0].id == fold )
4 == 1`, `CHECK( store.ActorSummaries("npc-1", true).size() == 1 ) 2 == 1`, `CHECK( shown[0].id == copyId ) 4 == 2`
(the condense, id 4, stayed shown). GREEN: `All tests passed (241 assertions in 28 test cases)`.

## 2. Abandon only when something will be recorded (I2)

- `HostedSession::RunTurn`: the abandon moved from after the request checks to immediately before
  `_store.CommitHostedTurn(record)`. The catch-up and fit-loop folds before it are written while the rollback is open
  and carry `made_in` (undo deletes them; abandon keeps them).
- `RecordEvents`: returns after the checks for an empty `events` list (it wrote nothing before either; the store
  call would now throw ContractError while a rollback is open), abandons otherwise.
- `ShareMemories`: abandons only just before `AddPendingShare` (not when the share is already stored) or before the
  first copy. For the copies, `ApplyShare` gained a `bool abandonsRollback` parameter (ShareMemories passes true,
  Maintain false); with it, the open rollback is abandoned before the first `AddActorSummary`, so a complete share
  with nothing left to copy leaves it open. The copies' clock is read before the abandon; the abandon changes no
  visible clock (hidden rows stay hidden; the shares it makes pending were already excluded from LatestClock as
  unapplied), so it is the same value.
- Docs (src/turn/HostedSession.h): the writers paragraph (when each one abandons, and what leaves the rollback open),
  RecordEvents ("An empty list records nothing"), RunTurn (a cancelled or throwing turn leaves an open rollback open),
  Rewind (what abandons), `AbandonRollbackForNewRecord` and `ApplyShare`.
- Tests (tests/hosted/HostedRewindTests.cpp, rewritten around a shared `PlayAndRewind` helper: c-open with Hulda's
  turns at 1.0 and 2.0, Hulda's share with Saadia pending since 1.0, c-ended started and ended at 1.0, rewound to 1.5):
  - "Every new record while a rollback is open abandons it": SECTIONs for events, StartConversation,
    SetParticipants, EndConversation, ShareMemories that stores a pending share (Hulda to Belethor), RunTurn; each then
    `Rewind(2.0)` is not `rewound`, restores 0 turns and turn 2 stays hidden. Replaces the old single-case test.
  - "A request that records nothing leaves the rollback open": SECTIONs for a turn cancelled through its stop token
    (stop requested before the call), a turn whose resolve call throws InferenceError, a refused turn (event without
    text), refused events (no text), no events, ending an ended conversation, a share already pending; each then
    `Rewind(2.0)` is `rewound`, restores 1 turn and shows turn 2.

RED: `morpheus_tests.exe "[rewind]"`: `test cases: 8 | 6 passed | 1 failed | 1 skipped`, `assertions: 264 | 252
passed | 12 failed` (`CHECK( forward.rewound )` false etc.). Per section (`-c`): cancelled, model call fails, no
events and already pending FAILED; the three refused requests already passed (Task 4 abandoned after the checks).
GREEN: `test cases: 8 | 7 passed | 1 skipped`, `assertions: 264 | 264 passed`.

## 3. An undo clears its saved marks (M2)

- `UndoRollback` sets `status = 'undone', saved_marks = '{}'`; RunStore.h's doc says so.
- Test "An undone or abandoned rollback keeps no saved marks": apply, undo, apply, abandon, then both rows read
  `{}` (undone, abandoned).

RED: `CHECK( rows.ColumnText(1) == "{}" )` with `"{"folded_through":{"c1":4},"remembered":[["c1","npc-1",4],
["c1","npc-2",4]],"shares":[]}" == "{}"`, `test cases: 1 | 1 failed`. GREEN: `[rollback]` all passed (248
assertions in 29 test cases).

## 4. An empty clock is refused (ledger, Task 1)

- `HostDataFromJson` (src/turn/HostedWorld.cpp): `"clock": ""` is `DataError` "host data at /clock: 'clock' must not
  be empty; a run without a clock omits it". HostedWorld.h's doc of HostDataFromJson says the clock, when present, is
  a non-empty string (and the comment's ragged wrap is re-wrapped).
- Test: in "A host clock must be a JSON pointer, ..." (`[hosted][clock]`), `CHECK_THROWS_AS(..., core::DataError)` and
  `CHECK_THROWS_WITH(..., ContainsSubstring("/clock"))`.

RED: `[clock]`: both checks FAILED "because no exception was thrown", `test cases: 2 | 1 passed | 1 failed`. GREEN:
`All tests passed (14 assertions in 2 test cases)`.

## 5. Tests and docs before merge

- Test "The run's summaries leave out the hidden ones": `HostedSummaries()` is 3 summaries, 1 (npc-1's 1-2) after a
  rollback to 2.7, 3 after the undo. No RED: it guards behaviour already right (the review asked for the missing
  test, not a fix); GREEN `[rollback]` 253 assertions in 30 test cases.
- RunStore.h's class comment lists `summary_supersessions`.
- The `Rewind` doc names the InferenceError the index sync can throw (an embedding), says the store has already
  rewound then, and that the next full sync (SyncHistory: a later rewind that changes what is hidden, a fold during a
  turn, Maintain's mark of an ended conversation, reopening the run) repairs the index.
- Architecture spec (M5): the `summaries` row lists its columns (`clock` for a shared copy, `hidden_by`, `made_in`)
  and then says when a summary is superseded, instead of "`superseded` is no longer a column ..." inside the column
  list; the version note's separate version-2 upgrade sentence is folded into the "2, 3, 4 or 5" one.

## Final verification

- `ctest --preset tests`: 100% tests passed out of 652 (after commit 5; also run before each earlier commit: 650/651).
- `cmake --build --preset asan`: no errors or warnings. `ctest --preset asan`: 100% tests passed out of 652.
- Owned game unchanged: `[turn]` 163 assertions / 16 cases, `[replay]` 509 / 7, `[context]` 236 / 29, all passed; no
  file under tests/turn, tests/replay, tests/context or tests/data changed.
- `[rewind]` 7 passed 1 skipped (the live [server] test), `[rollback]` 253 / 30, `[hosted]` 1377 / 121,
  `[hosted-server]` 1253 / 61, all passed. The reload fixture: "The reload fixture: a rewind hides ..." (in-process)
  and "The reload fixture plays over HTTP" pass; the live one skips (no server URLs).
- Build output: no warnings (MSVC /W4 /WX).
- Byte checks (non-ASCII, control bytes), 0 and 0 on every changed file: src/store/RunStore.cpp, src/store/RunStore.h,
  tests/store/RollbackStoreTests.cpp, docs/superpowers/specs/2026-11-16-rollback-design.md, src/turn/HostedSession.cpp,
  src/turn/HostedSession.h, tests/hosted/HostedRewindTests.cpp, src/turn/HostedWorld.cpp, src/turn/HostedWorld.h,
  tests/hosted/HostedWorldTests.cpp, docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md. No escape
  sequences were written.

## Deviations

- Item 2, ShareMemories copies: the brief says abandon "only on the path that writes (... copies)". A complete share
  can have nothing left to copy (all copied earlier), so the abandon sits in `ApplyShare` before the first copy,
  behind a new `bool abandonsRollback` parameter (private helper; Maintain passes false). No test drives the
  complete-share-with-copies path after a rewind (the brief's "a share that writes" section uses the pending path).
- Item 2 tests: no test-visible accessor of the open rollback exists, so every section checks through
  `Rewind(2.0)` (restores the hidden turn when open; restores nothing when abandoned), as the brief allows.
- Item 5's HostedSummaries test has no RED (tests existing behaviour).
- The commit trailer is the one the dispatch gave (`Claude Opus 5.5`).

## Concerns

- None blocking. While a rollback is open, RunTurn's catch-up (`RememberFold`) and its fit-loop folds
  (`FoldConversation`, which also moves the conversation's `folded_through`) are now written before the abandon; an
  undo deletes them and restores `folded_through` from the saved marks, as for Maintain's writes. The "cancelled" and
  "model call fails" sections exercise only the no-fold case (the scripted context is roomy); no test cancels a turn
  after a fold while a rollback is open.

## Review minors left

- M1: records with no observation of their own (events, ends, shares) right after a rewind are dated with the last
  visible clock, below the rewind time (owner decision).
- M3: `Rewind` re-syncs the whole index even when nothing changed.
- M4 beyond the doc: the in-memory index keeps hidden rows until the next full sync if SyncHistory throws (only
  documented).
- Can-wait ledger items: HostDataFromJson comment wrap in HostedWorld.h line 31-33 was re-wrapped as part of item 4;
  the rest stay (`_clock`/`_hasClock` comment alignment, the unreachable `isfinite` check, the untested "is not a JSON
  pointer" message, the CondenseActorSummaries comment, the open-time checks of `hidden_by` naming an undone rollback
  and of `made_in` / `unapplied_by`, the saved share `applied` range check, store refusal tests matching the method
  name, the HostedTestSupport doc comment, ServerTestSupport long lines, no fixture step checking the rewind answer's
  `rollback` field).
- Recommendation not acted on (outside the brief): the README note telling hosts to start a new conversation rather
  than end the one the rollback re-opened.

## Re-review follow-up (commit 0102883, on top of 38d6432)

From `.superpowers/sdd/e2-final-rereview.md`. This is "Path 1" plus the two minor rewraps, in one commit.

- The new test is "A share whose copies are written at once abandons the rollback; one with nothing to copy leaves it
  open" in tests/hosted/HostedRewindTests.cpp. Its steps:
  - Conversation `told`: Hulda's turn at 1.0, then the conversation ends and `Maintain` catches Hulda up.
  - Conversation `later`: Hulda's turn at 2.0.
  - `Rewind(1.5)` hides 1 turn.
  - `ShareMemories(Hulda, Saadia, told)`: shared 1, not pending; Saadia's copy is told by Hulda with `madeIn == 0`.
  - `Rewind(2.0)`: not rewound, 0 turns restored; `later` stays hidden and the copy stays.
  - Conversation `third`: a turn at 3.0, which `Rewind(2.5)` hides.
  - The same share again: shared 0, not pending.
  - `Rewind(3.0)`: rewound, 1 turn restored, and the `third` turn is shown again.
- First run, with the code as it is: `All tests passed (20 assertions in 1 test case)`.
- Second run, with the abandon removed for a moment: ShareMemories was changed to call `ApplyShare(..., false)`. The
  test fails: `test cases: 1 | 0 passed | 1 failed`, `assertions: 20 | 14 passed | 6 failed`. The failing checks:
  - `copies[0].madeIn == 0` read `1 == 0`.
  - `CHECK_FALSE(forward.rewound)` read `!true`.
  - `forward.restoredTurns == 0` read `1 == 0`.
  - `CHECK_FALSE(session.Conversation(later).has_value())` read `!true`.
  - `Memories(SAADIA).size() == 1` read `0 == 1`: the undo deleted the copy.
  - `again.shared == 0` read `1 == 0`.
- The `true` is back. `git diff` on src/turn/HostedSession.cpp was empty afterwards.
- Rewraps:
  - RunStore.h: the `ApplyRollback` doc, which had a 165-column line, and the `UndoRollback` doc, which had a
    121-column line from item 3.
  - HostedSession.h: the RunTurn doc, which wrapped raggedly at "Throws DataError naming the field for a", and its
    last paragraph.
  - HostedSession.h: the RecordEvents doc, which had a 134-column line from before this branch.
  - Only lines 34 and 36 of HostedSession.h are still over 120 columns: struct member lines from before this branch.
- Results:
  - `[rewind]`: 8 passed, 1 skipped (live).
  - `[hosted]`: 1397 assertions in 122 test cases passed.
  - `[rollback]`: 253 assertions in 30 test cases passed.
  - `ctest --preset tests`: 100% tests passed out of 653.
  - asan was not re-run; this commit adds one test and changes comments only.
  - Byte checks gave 0 and 0 on tests/hosted/HostedRewindTests.cpp, src/store/RunStore.h and src/turn/HostedSession.h.
