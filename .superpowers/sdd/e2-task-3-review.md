### Spec Compliance
- Spec compliant. Apply followed by undo restores the store exactly for every row the transactions touch. Each mark rule matches spec section 3. Traced cases are below.
- Deviations from the brief, all justified:
  - `saved_marks` gains `shares` and undo restores each share as saved. This is the controller's decision, now spec lines 70, 80 and 99.
  - The tail of `tests/store/HostedStoreTests.cpp:1530-1538` now expects `Open` to throw. The open-time two-open check required by spec "Errors" makes the old `AddActorSummary` path unreachable through `Open`.
  - The two-open message wording differs from the brief. It still names the file and `rollbacks` (diff 91).
- Cannot verify from diff:
  - The full index sync after apply and undo (spec 3, last bullet of apply and of undo) belongs to the session task.
  - Abandon-before-every-new-record (spec "Playing on") is also the session's job. The store's undo depends on it (see Minor 3).

Traced cases (diff line numbers refer to `review-59377fc..97fa99f.diff`):
- **Hidden rows.**
  - Apply sets `hidden_by` only where it is 0 (diff 154, 161, 165), so rows that an abandoned rollback hid keep their mark.
  - Undo clears only `hidden_by = id` (diff 287), so those rows stay hidden for good.
  - A summary made in R is never hidden by R, because apply's hiding runs once, before any write. Deleting `made_in` rows before unhiding (diff 284-288) is therefore safe.
- **Covering rule** (diff 176-184).
  - It runs after the turn update, so `MAX(number) ... hidden_by = 0` is the post-rollback `cut`.
  - In the fixture, npc-2's 1-4 is hidden (4 > 2) and npc-1's 1-2 stays.
  - A conversation started after T has `cut` 0, so all of its summaries are hidden.
  - Condenses use the run-wide maximum, as spec line 64 requires.
- **NPC mark** (diff 189-202). Each NPC's mark becomes `min(saved, cut)`, then `MIN(from_turn) - 1` over its own folds that R hid with `from_turn <= cut`.
  - Shared copies are excluded by `told_by = ''` and condenses by `conversation_id` matching.
  - When there are no such folds, `MIN` is NULL and `NULL - 1` is NULL, so `COALESCE` keeps the mark.
  - Restricting to `hidden_by = ?1` is right: folds that an abandoned rollback hid were already accounted for.
  - Fixture: npc-2 gets min(4, 2, 1-1) = 0. npc-1 gets min(4, 2) = 2, because its 3-4 starts after cut.
  - Post-abandon case: R1 at 2.7 is abandoned, turn 5 is played at 2.8, npc-1 folds 3-5, then R2 at 2.75 is applied. cut is 2, the fold 3-5 is hidden, 3 > 2 so it does not limit the mark, and the mark becomes 2. Correct.
- **Condense during an open rollback, then undo.**
  - `CondenseActorSummaries` (RunStore.cpp:1104-1124) adds a `made_in` row plus links whose `condensed_id` is that row. Pre-existing rows are not modified.
  - Undo deletes exactly those links (diff 280-283) before the summaries.
  - Links whose `summary_id` is a made-in summary can only point to a condense also made in R, because R is open for every such write. So deleting by `condensed_id` covers them, as the comment at diff 274-275 says.
- **Marks written while open.**
  - `FoldConversation`, `MarkFolded` and `RememberFold` change `folded_through` and `actor_memory`.
  - Undo rewrites every saved `folded_through` and replaces `actor_memory` wholesale (diff 303-317). `actor_memory` has exactly the three saved columns (RunStore.cpp:125-130), so the restore is exact, including NPC rows first created while open.
- **Shares.**
  - `MarkShareApplied` sets `unapplied_by = 0` (RunStore.cpp:1202-1211). So a reapply during R stops the share being pending, and abandon (diff 331) leaves it alone, which is correct.
  - Undo writes back `[applied, applied_clock, unapplied_by]` per saved id (diff 292-301). It covers:
    - unapplied by R and applied again (applied state restored);
    - pending at apply and applied during R (pending again);
    - a hidden late share (restored and unhidden).
  - The unapply statement filters `hidden_by = 0` (diff 169), so a share is never both hidden and unapplied. Abandon's reset therefore touches only visible shares.
  - At apply time no row has `unapplied_by <> 0`: undo restores the saved 0 and abandon clears it. So `unapplied_by = 0` in the filter loses nothing.
- **Open order.** The upgrade (RunStore.cpp:499-573) runs before the new hosted checks (diff 36-58), so v5 files have `hidden_by` columns by the time they are queried.

### Strengths
- The SQL matches the brief statement for statement. The comments explain each rule, including why a hidden fold "covers a visible turn" exactly when `from_turn <= cut` (diff 192-195).
- Shares in `saved_marks` close the two gaps the brief left. Each has a dedicated test:
  - `RollbackStoreTests.cpp` "pending at the rollback and applied while it is open" (diff 767-790);
  - "unapplied ... and applied again" (diff 792-823). This one also proves `applied_clock` survives by applying a second rollback at 4.0.
- `UndoRollback` validates the whole `saved_marks` shape before its transaction (diff 213-263). The damaged test checks 14 shapes, and for each one that nothing changed (diff 835-872).
- `OpenRollbackId()` now delegates to `OpenRollback()` (diff 357-373), so there is one reader and one message.
- Every `Step()` is checked. Statements are scoped before the next write.
- No new code outside the store yet calls the new API (grep over `src/` and `tests/`), so no other callers are affected.
- Byte check: 0 non-ASCII bytes in all four changed files.

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
1. **Two undo statements are unexercised.** No test condenses or embeds a summary while a rollback is open and then undoes it. Removing the `summary_supersessions` delete (diff 280-283) or the embeddings delete (diff 276-279) would leave every test green. The link table exists precisely for "a summary may be condensed again while a rollback hides its first condense" (spec line 68).
   - Suggested extension of "A rollback shows again what a hidden condense superseded" (diff 699-720): condense npc-1 again while open, put an embedding on the refold, undo, then assert:
     - the new links and the embedding are gone;
     - npc-1 shows only the original condense;
     - `RememberedThrough` is 4 again.
2. **Two hiding paths are untested at store level.** The fixture (diff 610-629) has no event after T and no conversation started after T. So the `conversation_events` and `conversations` entries of the hide loop (diff 153) are unexercised here, as is a hidden conversation's turn count. The later reload fixture covers events, but nothing covers a hidden conversation with everything in it (spec line 62).
3. **Undo silently leaves rows created while open.** Undo restores only the saved ids and keys (diff 292-317). A share, conversation or turn added while open (a broken abandon-first contract) would survive the undo, and nothing reports it. The report names this concern.
   - Fail-fast options: undo could `MORPHEUS_REQUIRE` that no `pending_shares` row has an id beyond the saved ones; or the hosted record writers could require no open rollback.
   - At minimum, the session task's brief should state the contract.
4. **Open misses a few damaged-row cases.** The open-time check (diff 47-58) catches `hidden_by` naming no rollback. It does not catch:
   - `hidden_by` naming an `undone` rollback, which undo always clears, so such a row is damaged too;
   - `made_in` or `unapplied_by` naming no open rollback.

   This is beyond the spec's "Errors" list, so it is optional.
5. **The `applied` field is not range-checked.** The saved share's `applied` is checked only as an integer (diff 257-259), not as 0 or 1. This is trivial.

### Assessment
**Task quality:** Approved
**Reasoning:** Apply then undo restores every touched table exactly (hidden marks, refolds with their links and vectors, share states, folded and remembered marks), and each mark rule matches spec section 3 in the traced cases. The remaining items are test coverage of two undo statements and of two hiding paths, plus a contract the session task must uphold.
