# Task 3 report: the rollback transactions in the store

Status: DONE_WITH_CONCERNS (one Task 2 test changed to match the new open-time check; see Deviations)

Commit: 97fa99f "Rollback transactions: apply hides and moves marks back, undo restores exactly, abandon keeps the
hidden rows hidden"

## History

My first pass stopped with NEEDS_CONTEXT. The brief's undo did not revert a share that Maintain applied while a rollback
was open:
- Case S: a share pending at apply time and applied during the rollback stayed applied with no copies.
- Case 1: a share reapplied during the rollback lost its original `applied_clock`.

The coordinator chose fix 1 and asked for:
- every pending_shares row's `[id, applied, applied_clock, unapplied_by]` saved in `saved_marks` and restored exactly
  on undo (this replaces the brief's `unapplied_by = 0` statement);
- tests for both cases;
- stricter checks of `saved_marks` and of every `Step()` result.

All of that is implemented.

## What I implemented

### `src/store/RunStore.h`

- `struct Rollback { id, gameTime }` and `struct RollbackCounts { hiddenTurns, restoredTurns }`, with comments.
- Public `OpenRollback()`, `ApplyRollback(double)`, `UndoRollback()` and `AbandonRollback()`. Their comments follow
  spec section 3.
- `Open`'s comment names the new open-time errors.

### `src/store/RunStore.cpp`

**`OpenRollback()`**
- Returns the one open row.
- More than one open row is a StoreError: "'<file>': rollbacks holds more than one open rollback (a and b)".
- The private `OpenRollbackId()` now returns `OpenRollback()`'s id, or 0 when none is open. Both share one source and
  one message.

**`ApplyRollback`**
- The brief's SQL, plus `"shares"` in `saved_marks`: every row as `[id, applied, applied_clock or null, unapplied_by]`.
- Every `Step()` result is checked with `MORPHEUS_REQUIRE`.
- Statements are scoped so each one finishes before the next write.

**`UndoRollback`**
- Reads `saved_marks` and checks its whole shape before the transaction:
  - an object;
  - `folded_through`: an object of integers;
  - `remembered`: `[string, string, integer]` triples;
  - `shares`: `[integer, integer, number or null, integer]`.
- Any fault throws StoreError "'<file>': rollback N: rollbacks column 'saved_marks' <problem>" and changes nothing.
- In the transaction, the brief's deletes and unhides, then every saved share's
  `applied` / `applied_clock` / `unapplied_by` is written back. This replaces
  `UPDATE pending_shares SET unapplied_by = 0 WHERE unapplied_by = ?1`.
- Then the saved marks are restored exactly and the status becomes `undone`.

**`AbandonRollback`**: as the brief has it.

**`Open`** now builds the `RunStore` first, then runs the hosted checks on it:
- the existing actor_memory orphan check;
- a check on each of the six tables that every non-zero `hidden_by` names a row in `rollbacks`;
- `OpenRollback()`, which throws when more than one rollback is open.

### `tests/store/RollbackStoreTests.cpp`

- The brief's seven tests. The damaged test now loops over 14 damaged `saved_marks` texts: not JSON, not an object,
  and each missing or wrong-shaped part, including `shares`. For each one it also checks that nothing changed.
- Two new tests:
  - Case S: a share pending at apply time, applied while the rollback is open (its copy carries `made_in`), then
    undone. It is pending again with no applied clock and `unappliedBy` 0, and the copy is gone.
  - Case 1: a share applied at 4.6 with a copy, unapplied by a rollback to 2.7, reapplied at 2.5 with a new copy, then
    undone. No share is pending, and the only copy is the original at 4.6. A later `ApplyRollback(4.0)` unapplies the
    share (`appliedClock` 4.6, `unappliedBy` = the new rollback) and hides the copy.

## TDD evidence

RED: `cmake --build --preset tests` gave

    RollbackStoreTests.cpp(113,18): error C2039: 'RollbackCounts': is not a member of 'morpheus::store'
    RollbackStoreTests.cpp(113,49): error C2039: 'ApplyRollback': is not a member of 'morpheus::store::RunStore'
    RollbackStoreTests.cpp(115,5): error C2039: 'OpenRollback': is not a member of 'morpheus::store::RunStore'

First GREEN attempt for `"[rollback]"`: 22 of 23 test cases passed. Task 2's "A summary written while a rollback is open
is made in it; two open rollbacks are damaged saved data" failed: `RunStore::Open` threw "rollbacks holds more than one
open rollback (3 and 4)" (see Deviations).

GREEN after updating that test (clean build, no warnings):

    "[rollback]": All tests passed (190 assertions in 23 test cases)
    "[store]":    All tests passed (638 assertions in 76 test cases)
    "[hosted]":   All tests passed (1207 assertions in 117 test cases)
    ctest --preset tests: 100% tests passed out of 637 (the usual 5 [server]/llama-server tests skipped)

## Byte checks

`LC_ALL=C grep -c $'[\x80-\xFF]'` and the control-byte grep each printed 0 and 0 for all four files:
- `src/store/RunStore.h`
- `src/store/RunStore.cpp`
- `tests/store/RollbackStoreTests.cpp`
- `tests/store/HostedStoreTests.cpp`

## Files changed

- src/store/RunStore.h
- src/store/RunStore.cpp
- tests/store/RollbackStoreTests.cpp (new)
- tests/store/HostedStoreTests.cpp (one test's tail)

## Deviations from the brief

1. **Shares in `saved_marks`** (coordinator's fix 1). `saved_marks` gains `"shares"`, and undo restores each share's
   application exactly instead of clearing `unapplied_by`. Spec text is the coordinator's to update; docs/ is untouched.
2. **Stricter checks** (CLAUDE.md, confirmed by the coordinator). The undo validates the whole `saved_marks` shape before
   the transaction, and every `Step()` result is checked.
3. **Task 2's test updated** (`tests/store/HostedStoreTests.cpp`). It opened a run holding two open rollbacks and expected
   `AddActorSummary` to throw. The brief's open-time check (spec, "Errors") now refuses that run at `Open`, so the test's
   tail asserts that `Open` throws a StoreError naming run.db and rollbacks. This file is outside the brief's commit
   list. I included it under CLAUDE.md's rule to update callers to the new contract.
4. **Message wording.**
   - The two-open message keeps Task 2's wording ("rollbacks holds more than one open rollback (a and b)") rather than the
     brief's "rollbacks: more than one rollback is open". Both name `rollbacks`.
   - The `saved_marks` message names "rollbacks column 'saved_marks'".
5. **Restoring `folded_through`** iterates the saved object with explicit iterators and binds integers as `std::int64_t`.
   Same meaning as the brief.

## Concerns

- The spec's description of `saved_marks` needs the `shares` part. The coordinator said they would update it.
- The undo assumes no share row is added while a rollback is open: Task 4 must abandon the rollback before
  `AddPendingShare`. If a share were added while open, undo would leave it as it is. Nothing in the store enforces this,
  because the brief's contracts do not ask for it.
