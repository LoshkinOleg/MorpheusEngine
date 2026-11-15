# Sub-project E2: re-review of the final fix round

Range: 9292990..38d6432 (5 commits). Inputs: `.superpowers/sdd/e2-final-fix-brief.md`,
`.superpowers/sdd/e2-final-fix-report.md`, the review `.superpowers/sdd/e2-final-review.md`. Read-only; no source file
edited.

Verification during this re-review: the Debug test binary is current (built 15:24; the last commit, 15:25, holds only
docs and the HostedSummaries test, and `[rollback]` shows its 30 cases). `"[rollback],[rewind],[hosted],[hosted-server],
[store],[clock]"`: 223 passed, 1 skipped (live), 2940 assertions. `"[turn],[replay],[context]"`: all 52 cases pass.
The fixer reports `ctest --preset tests` and `ctest --preset asan` 652/652; I did not re-run asan.

## I1 (a condense holding hidden content stays visible): holds

- `RunStore.cpp:1556-1572`: after the clock and turn-range hides and before the marks move, a loop hides every visible
  condense linked from a summary hidden by this rollback, repeating until `changes()` reads 0. The IN subquery is not
  correlated, so each pass goes one level down the chain, and the loop ends because each pass that changes rows hides at
  least one more. `changes()` counts only the top-level UPDATE on this connection, which is what is wanted.
- Sound across the session and the store:
  - A summary this rollback hides was written after its time (it covers a hidden turn, has a later clock, or is itself
    such a condense). Any condense linking it was written later still, so hiding the condense is never wrong.
  - A condense hidden this way brings back the summaries it superseded. Those that cover hidden turns were already
    hidden by the existing rules, so nothing is shown that should not be.
  - The marks are untouched: the mark rules read only conversation-set, non-copy folds. Scenario B's test checks that
    the mark moves to 0 and comes back to 6.
  - Undo shows the condense again (same `hidden_by`), and abandon keeps it hidden.
  - Maintain while the rollback is open writes new condenses, which link only visible summaries (`made_in`). The undo
    deletes those links. Nothing of the rollback's hidden set gets re-linked.
- Both review scenarios are store tests (`RollbackStoreTests.cpp`: a hidden shared copy under a condense; a nested
  range). Each checks what is shown after the apply and after the undo. The spec's section 3 bullet is updated.

## I2 (abandoning before anything is recorded): holds

- RunTurn abandons immediately before `CommitHostedTurn` (`HostedSession.cpp:421-425`). Nothing between the old place
  and the commit is refused while a rollback is open:
  - The catch-up and fit-loop writes (`RememberFold`, `FoldConversation`, `CondenseActorSummaries`) get `made_in` from
    `AddActorSummary`.
  - The marks they move are restored from `saved_marks` on an undo.
  - `NextHostedTurn` is read before the abandon and is unaffected by it.
- RecordEvents returns before abandoning for an empty list. Without an open rollback this is the same behaviour as
  before (the old store call was a no-op), and the checks still run first.
- ShareMemories abandons only before `AddPendingShare`, or before the first copy inside `ApplyShare(..., true)`.
  Maintain passes false. The copies' clock is read before the abandon; this is correct because abandoning changes no
  visible clock:
  - The shares it touches had `unapplied_by` set, so they were already left out of `LatestClock`.
  - Afterwards their `applied_clock` is NULL.
  - Clearing `made_in` changes no clock.
- The first copy is inserted after the abandon, so every copy is ordinary (`made_in` 0).
- Tests cover every writer abandoning (6 sections) and seven requests that leave the rollback open: cancelled, model
  failure, refused turn, refused events, no events, ending an ended conversation, share already pending.

## Did the fix break anything?

No regression found. The owned game is unchanged (no owned code touched; its suites pass). The `ApplyShare` flag is a
private helper with two callers, which the coding rules allow. The `UndoRollback` change (`saved_marks = '{}'`) is safe:
only the open rollback's marks are ever read, and `Open` does not validate undone rows. The empty-clock refusal sits in
`HostDataFromJson`. A stored host data file never holds `""`, because `HostDataToJson` omits an empty clock, so
reopening an existing run is unaffected.

## The two untested paths

1. **A share whose copies are written at once after a rewind: needs a test before merge.** This path has no store-side
   safety net: `AddActorSummary` is allowed while a rollback is open. If the `abandonsRollback` abandon were dropped or
   misplaced, the copies would be quietly tagged `made_in`, and the next roll-forward would delete them. The host's
   share would be lost with no error. The only guard is the new flag, and it is untested in both directions.
   - Test, in `HostedRewindTests.cpp`:
     - Before the rewind: end a conversation the teller witnessed, then run Maintain so the teller catches up.
     - After the rewind: share it. Check that `shared > 0` and that the copies have `madeIn == 0`, and that a
       `Rewind` to the old latest time restores nothing.
     - Then share the same thing again with nothing left to copy, after a fresh rewind, and check that the rollback
       stays open.
2. **A turn cancelled after a fold was written while a rollback is open: can wait.** The parts are already covered:
   - Store tests cover `made_in` tagging, an undo deleting the made-in summaries with their embeddings, and the saved
     `folded_through` and NPC marks being restored exactly.
   - The reload fixture covers a catch-up refold while the rollback is open, then an undo.
   - A regression back to the early abandon is caught by the existing "cancelled" section.

   A session test that forces a fit-loop fold (a small budget) before cancelling would add confidence but is not
   needed for merge.

## Minor

- `RunStore.h:284`: the ApplyRollback doc line is 165 columns; the new clause was not re-wrapped. `HostedSession.h`:
  the RunTurn doc wraps raggedly at "Throws DataError naming the field for a". Cosmetic.
- Left by the fixer, as agreed: M1 (records without an observation dated below the rewind time; owner decision), M3
  (Rewind re-syncs when nothing changed), M4 beyond the doc, the can-wait ledger items, and the README note for hosts.

## Assessment

**Ready to merge? With one fix:** add the test for path 1, the share whose copies are written at once after a rewind.

**Reasoning:** I1 and I2 now hold in the store, the session, Maintain and an undo or abandon, and nothing regressed.
The one remaining gap is the new `abandonsRollback` path, the only abandon point without a store-side guard; it should
be tested before merge.
