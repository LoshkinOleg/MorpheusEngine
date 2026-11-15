# Sub-project E2 (rolling a hosted run back): final whole-branch review

Range: eb08d54..9292990 (12 commits). Requirements: `docs/superpowers/specs/2026-11-16-rollback-design.md` (HEAD),
`docs/superpowers/plans/2026-11-16-rollback.md`, `.superpowers/sdd/e2-global-constraints.md`, `CLAUDE.md`.
Reviewer: final whole-branch review (read-only; no source file edited).

Verification run during the review (the Debug test binary built after 8c7aeaf; later commits change only live tests and
docs): `morpheus_tests "[rollback],[rewind],[store],[hosted],[hosted-server]"`: 218 passed, 1 skipped (2811
assertions). `morpheus_tests "[turn],[replay],[context]"`: all 52 cases pass; no file under tests/turn, tests/replay,
tests/context or tests/data changed. asan was not run by me (the ledger says it was not run after Task 4 either).

## Strengths

- The materialized design is carried through consistently: one `hidden_by` vocabulary over six tables, readers filter it
  in SQL (`RunStore.cpp` readers at 954-1300), and `Open` refuses a `hidden_by` naming no rollback and two open
  rollbacks (`RunStore.cpp:626-637`).
- `ApplyRollback` (`RunStore.cpp:1470-1576`) is one transaction in a sound order: save, hide by clock, unapply shares,
  hide summaries by their conversation's (or the run's) last visible turn, then move the marks with the already-updated
  visibility. Two-argument `MIN` scalar use and the `COALESCE(..., 0)` for an all-hidden conversation are right.
- `UndoRollback` validates the saved JSON whole before touching anything (`RunStore.cpp:1584-1634`) and restores marks
  by replacing `actor_memory` outright, so rows created by catch-ups while open cannot survive an undo.
- The controller's mid-flight decisions hold up under composition: the `summary_supersessions` link table makes "a
  condense while a condense is hidden" correct (the old single `superseded_by` would have shown a memory twice), and
  saving every share's applied state makes "unapplied, re-applied while open, then undone" exact (test at
  `RollbackStoreTests.cpp:360`).
- The store's writers of new records refuse while a rollback is open (`RequireNoOpenRollback`), so a session writer that
  forgot to abandon fails loud instead of writing a record an undo would orphan.
- Turn numbers: `NextHostedTurn` over every row plus AUTOINCREMENT on summaries means a hidden turn's or a deleted
  refold's embedding row can never be picked up by a new record with the same ref id.
- The v5 to v6 upgrade's supersession heuristic ("the actor's first condense-shaped summary after it") is exact for
  v5 data: a v5 condense superseded every shown summary of its actor at that moment, so the first later condense is the
  one. The "new file and upgraded file have the same tables, indexes and columns" test is a good guard.
- Composition with Maintain is right in the cases I traced: catch-up folds, condenses and share applications while a
  rollback is open all carry `made_in` / saved share state; an undo deletes them with embeddings and supersession links;
  an abandon makes them ordinary. Rewind is an ordinary worker job, so it never races Maintain.
- Morpheus's own game: owned code paths are untouched (only the schema SQL and new `Bind` overloads, every caller in
  `RunStore.cpp`), and the owned suites pass unchanged.

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

**I1. A condense that holds hidden content stays visible: NPCs keep remembering what the rollback hid.**
- File: `src/store/RunStore.cpp:1545-1555` (summary hiding), with `src/turn/Folding.cpp:86,106` (a condense's range is
  `summaries.front().fromTurn .. summaries.back().toTurn` of the shown summaries ordered by `from_turn`).
- What is wrong: a condense (`conversation_id = ''`) is hidden only when its `to_turn` is past the run's last visible
  turn. Its content can come from a summary the same rollback hides for another reason, and then the condense stays.
  - Scenario A (shared copy, sequential conversations, monotonic clock): c1 (Hulda, player) turns 1-3 at clocks 1-3,
    ends; Hulda remembers 1..3. c2 (Saadia, player) turns 4-5 at clocks 4-5, ends; Saadia remembers 4..5. c3 (Belethor,
    player) turn 6 at clock 6, turn 7 at clock 7. At clock 7 the host shares Saadia's c2 memory with Hulda: the copy
    (4..5) gets clock 7. Hulda then speaks in c4 at clock 8 (turn 8) and her upkeep condenses [1..3, copy 4..5] into C
    (1..5). The player reloads a day-6.5 save: turns 7 and 8, c4 and the copy (clock 7) are hidden; C has no clock and
    to_turn 5 <= 6, so it stays visible, and fold 1..3 stays superseded by it. Hulda's prompt (and her retrieval) shows
    C, which contains what Saadia told her at day 7. If the share had gone through `pending_shares`, Maintain re-applies
    it while the rollback is open (the copy is hidden, so `ApplyShare` copies again) and Hulda holds the story twice.
  - Scenario B (interleaved conversations, which the engine allows and the v4 upgrade code already anticipates): c1
    (Hulda) turns 1,2,5,6; c2 (Saadia) turns 3,4. Hulda's fold is 1..6; Saadia's 3..4 is shared to Hulda (copy 3..4).
    Hulda's shown summaries ordered by from_turn are [1..6, 3..4], so her condense is 1..4. A rollback to turn 4's clock
    hides 5,6 and the fold 1..6 (to_turn 6 > c1's last visible 2) and moves Hulda's mark to 0, but C (to_turn 4 <= 4)
    stays visible: Hulda shows the content of turns 5-6 and also gets a refold of 1..2.
- Why it matters: it breaks R6's acceptance ("NPCs neither show nor remember" what came later) in long play, where
  condensing and sharing are routine. The reload fixture never condenses, so no test sees it.
- Fix: after the existing hides, hide transitively (loop until no row changes, a condense can itself be condensed)
  every visible summary linked in `summary_supersessions` from a summary hidden by this rollback:
  `UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND id IN (SELECT l.condensed_id FROM summary_supersessions l
  JOIN summaries s ON s.id = l.summary_id WHERE s.hidden_by = ?1)`. This is always right: a condense written after a
  summary the rollback hides was written after the rollback's time too, and its own superseded summaries come back by
  the existing rule. It also makes Scenario B correct without changing E1's condense range. Update the spec's
  section 3 bullet ("every NPC summary covering a hidden turn") to name it, and add store tests for both scenarios
  (a hidden shared copy under a condense; a nested range).

**I2. A turn request abandons the hidden future before anything is recorded; a cancelled or failed turn loses it.**
- File: `src/turn/HostedSession.cpp:281-282` (abandon after the request checks), commit at `:420`. The plan mandated
  this (`plan:957`, "RunTurn once its request passed its checks"), but the spec says the first new record, "a turn
  committed", abandons (spec section 3, "Playing on abandons the hidden future").
- Scenario: the player loads a day-5 save (rewind to 5), Mantella sends a turn, and the player immediately reloads the
  later save: the host cancels the in-flight turn (or llama-server times out with an InferenceError, or the fit loop
  throws TurnError on an over-long tail). Nothing was committed, yet the rollback is abandoned, and the rewind to the
  later save's time restores nothing: Morpheus's memory now disagrees with the save for good. Same, less likely:
  `ShareMemories` when the share is already stored as pending (`:537`, abandons and writes nothing) and `RecordEvents`
  with an empty `events` array (`:182`; the route accepts `{"events": []}`).
- Fix: in RunTurn, call `AbandonRollbackForNewRecord()` immediately before `_store.CommitHostedTurn(record)`. Nothing
  before the commit is refused while a rollback is open (catch-up, fit-loop folds and condenses go through
  `AddActorSummary` / `RememberFold` / `FoldConversation` / `CondenseActorSummaries`, none of which require no open
  rollback); those writes then carry `made_in`, so an undo deletes them if the turn never commits and the abandon makes
  them ordinary if it does, exactly like Maintain's. In ShareMemories abandon only when a pending row or copies will be
  written; in RecordEvents only when `events` is not empty (or refuse an empty list). Tests: a turn cancelled (stop
  requested) after a rewind leaves the rollback open and a later rewind restores everything; a refused request (bad
  speaker) leaves it open (the ledger's Task 4 minor).

### Minor (Nice to Have)

- **M1. Records with no observation of their own are dated below the rewind time.** `RunStore::LatestClock`
  (`RunStore.cpp:1283-1300`) is the highest visible record's clock. Right after a rewind to 5 whose last visible record
  is at 3, an end, events or a share recorded before any new clocked record are dated 3. Scenario: load the day-5 save,
  record events (abandons, dated 3), play on, later load a day-4 save from the old timeline: the rewind to 4 keeps
  those events, which happened after day 5. This follows the spec's definition (section 1), so it is an owner
  decision: counting the latest non-undone rollback's game time as "seen" closes it.
- **M2. Undone rollbacks keep their `saved_marks`.** `UndoRollback` (`RunStore.cpp:1689`) leaves the O(run) JSON (every
  NPC mark and every share ever) in each undone row; Abandon clears it. The host rewinds on every load (death reloads
  included), and each rewind with an open rollback adds a row, so the file grows by that JSON per reload. Clear it to
  `'{}'` on undo as abandon does.
- **M3. `Rewind` re-syncs the whole index even when nothing changed** (`HostedSession.cpp:590`: no rollback open and
  the time at or past the latest clock). Loading the latest save then re-reads every conversation. Return before
  `SyncHistory` when `!result.rewound`.
- **M4. If `SyncHistory` throws after the store rewound** (embedder down), the in-memory index keeps hidden turns and
  memories until the next sync (`MemoryIndex::Replace` runs only after all vectors). Usually no embed call is needed
  (hidden rows' vectors are cached), so low risk; the Rewind doc should at least name InferenceError (ledger minor).
- **M5. Docs:** `RunStore.h:59-60` table list lacks `summary_supersessions`; the architecture spec's `summaries` row
  reads "`superseded` is no longer a column since version 6: ..." inside a column list, and its version note repeats
  the version 2 upgrade sentence before the "2, 3, 4 or 5" one.

## Ledger minors: triage

Fix before merge (cheap, and each guards a contract of this branch):
- Task 1: `"clock": ""` is silently "no clock" (`HostDataFromJson`, `HostedWorld.cpp`), so a host that sends an empty
  pointer finds out only when a rewind is refused. Refuse it as a DataError at `/clock` and test it (fail fast).
- Task 2: `HostedSummaries` has no hidden-row test of its own. It feeds the retrieval index (`SyncMemories`), so a
  regression there leaks hidden memories into prompts; add it with I1's tests.
- Task 2: the `RunStore.h` table list lacks `summary_supersessions` (one line, see M5).
- Task 4: only RecordEvents (and RunTurn via the fixture) are tested to abandon: add StartConversation,
  SetParticipants, EndConversation and ShareMemories (a missing abandon would reach the host as a 500 ContractError).
- Task 4: no test that a refused request leaves the rollback open: part of I2's fix.
- Task 4: RunTurn cancelled after abandon / ShareMemories already pending abandons without writing: promoted to I2.

Can wait:
- Task 1: HostDataFromJson comment wrap (`HostedWorld.h:31-33`); `_clock`/`_hasClock` comment alignment; the
  `isfinite` check unreachable from parsed JSON (harmless, programmatic JSON can hold infinities); the "is not a JSON
  pointer" message untested.
- Task 2: CondenseActorSummaries comment reads as two fragments.
- Task 3: open-time checks of `hidden_by` naming an undone rollback, and of `made_in` / `unapplied_by` naming no open
  rollback; the saved share `applied` not range-checked. Damaged-data detection only; nothing writes such rows.
- Task 4: store refusal tests match the method name rather than "a rollback is open"; HostedTestSupport doc comment
  ragged; Rewind doc omits InferenceError (M4).
- Task 5: ServerTestSupport leave/join lines ~130 columns; no fixture step checks the rewind answer's `rollback` field
  (ApiRewindTests checks it at the route).
- Task 6: architecture version note redundant (M5).

## Other checks (no finding)

- Apply / undo / abandon / Maintain: a share unapplied by the rollback and re-applied by Maintain while open, then
  undone, then abandoned, never ends with copies twice or a share lost (copies and application share one clock, so
  "unapplied" and "its copies hidden" coincide). A share created after the time is hidden with its row; after abandon it
  stays hidden by design.
- Marks: after an apply every `folded_through` and NPC mark is at most its conversation's last visible turn, and an
  NPC's mark is at most `fromTurn - 1` of its own hidden fold that covers a visible turn; after an undo both are the
  saved values exactly; after an abandon new turns are numbered past every hidden one, so no mark points at a hidden
  number that a new turn could reuse.
- Hidden-forever turns sitting between visible ones after an abandon are handled: `Entries` follows only visible turns,
  so events recorded after the last kept turn are placed before the first new one, as they happened.
- The v5 to v6 upgrade on real runs: `DROP COLUMN` needs SQLite >= 3.35 (3.50.4 is fetched); none of the dropped
  columns is indexed; null clocks keep every pre-upgrade row visible forever; v5 never kept applied shares, so every
  existing `pending_shares` row is rightly pending.
- The route: body checks, 400 for no clock / bad `game_time`, 409 for a run not open, and the net counts and
  `rollback` field are covered by `ApiRewindTests.cpp`.

## Recommendations

- Treat the condense as the unit of hiding for derived memories: anything derived from a hidden summary is hidden (I1).
  The same rule would cover any future derived row (e.g. an F prompt-pack digest).
- Check the host's housekeeping on load before the live test with Mantella: if it ends the conversation that was open
  in the save (the rollback re-opened it), that end abandons the rollback, and roll-forward never works in practice.
  The README could tell hosts to start a new conversation rather than end the old one after a rewind.
- Run `ctest --preset asan` on the branch before merge (not recorded in the ledger since Task 4).

## Assessment

**Ready to merge? With fixes.**

**Reasoning:** The store transactions, the session's abandon/undo/Maintain composition, the upgrade and the server
route are sound and well tested, and the owned game is untouched; but a condense can keep hidden content visible (I1),
which breaks R6's "NPCs neither remember" in ordinary long play, and a turn request abandons the hidden future before
anything is committed (I2), contrary to the spec. Both fixes are small and local.
