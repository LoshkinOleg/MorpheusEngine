# Final review: final pass (G3 hosted review, E2 follow-ups, leftovers), 36fee24..854060f

Reviewer: Opus, read-only. Inputs: the range package `review-36fee24..854060f.diff`, the plan
`docs/superpowers/plans/2026-11-16-final-pass.md`, the G3/E2/G2 specs, CLAUDE.md, the "Final pass" section of
`progress.md`. Code outside the range was read only to check named risks (SlotBackend, HttpServer's stream poll,
SyncHistory, ParseReviewOutput and BuildReviewGrammar, the LatestClock callers, the settings defaults).

## Strengths

- **The G3 turn flow matches the spec.** `HostedSession::RunTurn`, 3b:
  - With `review` the speak call gets an empty `TokenCallback`.
  - The review call matches the spec: tag `review`; grammar `BuildReviewGrammar(true, false)`; `review_max_tokens`
    and `review_thinking_tokens`; resolve sampling with `CallKind::Review` seed 0; standalone prompt.
  - `review.consistent == false` is the only respeak trigger. It cannot fire with no problems, because both the
    grammar and `ParseReviewOutput` require 1-3 problems when the verdict is inconsistent.
  - The respeak copies the speak spec. Only the tag and the `Renarrate` seed change.
  - Problems are dropped from the end to fit `narrate_allowance`, counted with the same `CountTokens` as
    `checkSpeaker`. So the plain-speak fallback always fits.
  - Every call checks `cancelled`.
  - The committed `record.line` is the delivered line. So the L3 entry, the fold and per-speaker memory all see the
    respoken line.
- **An unreviewed turn is unchanged:** two calls, streamed. Morpheus's own game is untouched apart from the call-tag
  constants, which spell the same strings.
- **Disconnects are still detected during a reviewed turn.** A reviewed turn has a long silent gap between `action`
  and the first `token`. HttpServer's 50 ms `STREAM_POLL` still lets cpp-httplib check the client, so a client that
  leaves during the review or respeak cancels the turn. Nothing is committed (the stop tests cover this at the session
  level).
- **Strong tests in `HostedReviewTests.cpp`:**
  - no review;
  - consistent (the call spec and the exact prompt);
  - inconsistent (exact respeak message, the first line never delivered, `callsAtFirstPiece == 4`);
  - unreadable;
  - the three allowance cases (all dropped, partial, plain);
  - the pack's text;
  - a stop during the review and a stop during the respeak;
  - no player text.
- **E2 is small and correct.**
  - `LatestClock` adds `SELECT game_time FROM rollbacks WHERE id = (SELECT MAX(id) ... status <> 'undone')`. Using
    the most recent rollback (by id), not the highest time, is right: an old abandoned 2.2 does not lift a later
    rewind to 1.0.
  - Every caller calls `AbandonRollbackForNewRecord()` before `LatestClock()` (RecordEvents, EndConversation,
    AddPendingShare). The abandoned rollback's time then dates the record, as the spec says.
  - Rewind computes `latest` after the undo, so the undone rollback never counts.
  - The early return on `!rewound` is exact: `rewound` is true whenever anything was undone or applied.
  - `RollbackStoreTests` walks open -> undone -> abandoned -> newer undone, and the two pins updated in the range are
    correct.
- **Task 5 is mostly clean.**
  - `CallTag.h` is used at every producer (Session, HostedSession, MemoryUpkeep) and every reader (CallKindOf,
    SeatLabelOf, HostedTestSupport). A grep of `src/` and `tools/` finds no tag literal left.
  - The `thinking_format` branch order is string, then not-an-object (the preset message, value as JSON), then
    object. It is tested with `5` and `null`.
  - The empty-`after_end` guard mirrors `skip`. Its test checks the request bodies, so it would fail without the
    guard.
  - The settings-editor test checks the file and both loads.
  - The README budget and `thinking_format` sentences landed as planned.
- **The docs agree with the code** on the turn body, `done`'s timings, API 1.5.0, the eighth pack text, the unknown-text
  list and the rollback rule, in the README `morpheus_server` section, CHANGELOG 1.5.0, the server spec, the
  prompt-pack spec and the rollback spec. The controller's `llama-server` README fix is right (settings.json spells
  `"llama-server"`).

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

**I1. The review call wipes the conversation slot's prompt cache. The respeak and the next turn then start from an
empty cache, and no doc says so.**

- Where:
  - `src/turn/HostedSession.cpp:545-548`;
  - spec `2026-11-16-hosted-review-design.md:28` ("the conversation's slot");
  - `README.md:307`;
  - `CHANGELOG.md:13`.
- What is wrong:
  - `reviewSpec.slot` stays -1, so SlotBackend puts the review on the conversation's slot.
  - The review prompt is standalone (its own system message, then the observation). It shares no prefix with the
    conversation prompt, so llama-server replaces the slot's cached prompt.
  - Two calls then pay for that:
    - The respeak right after the review re-evaluates the whole conversation prompt.
    - So does the next turn's resolve call. With Qwen3.5 and `--checkpoint-min-step 0` this includes the saved
      checkpoints, which are lost once the prefix diverges.
  - That is unless the llama-server build keeps a host-memory prompt cache (`--cache-ram`), which nothing in the
    README relies on or mentions.
- Why it matters:
  - The server spec gives each conversation its own slot "so alternating conversations keep their cached prompts".
    R2 (more than 80% of the prompt reused) and R4 (first sentence within 1.5 s) were measured on that basis.
  - R9 asks to measure the review's cost before enabling it, and the README says the cost is "one review call and
    sometimes one more speak call". But the biggest cost lands in the *next* turn's `timings.resolve` (`reused`
    drops), where a host measuring `timings.review` will not look.
  - The integration summary for the Mantella implementer will repeat whatever the README says.
- How to fix: decide one of these.
  - (a) Minimum, before merge:
    - Say in the README Review paragraph, CHANGELOG 1.5.0 and spec section 2 that the review replaces the
      conversation slot's cached prompt, so a respeak and the next turn re-evaluate the conversation prompt.
    - Have the live `[server]` test WARN the reviewed turn's `timings.review` (prompt_ms, generate_ms, consistent),
      and the next turn's `timings.resolve.reused`, if the fixture order allows one.
  - (b) Later design choice for the owner: keep the cache warm.
    - Either make the review a continuation of the speak prompt (resolve prompt + resolve output + speak message +
      the line + a review user message). This shares the slot's prefix, but the pack's `review` text would no longer
      be a system message.
    - Or route the review to another slot. Slot 0 is shared with every run's folds, so a review there can queue
      behind a fold.

**I2. The `[hosted-server]` ordering test does not test the ordering it claims.**

- Where: `tests/server/ApiReviewTests.cpp:50`.
- What is wrong:
  - `CHECK(firstToken > 0)` with the comment "the line is held back until the review is done" holds just as well on
    an unreviewed turn: the `action` event is always first.
  - The Task 3 minor ("ApiReviewTests lack ordering ... cases") was meant to be closed by Task 5, and it is not.
  - Engine-level ordering is proven in `HostedReviewTests` (`callsAtFirstPiece == 3`). The server-level check is
    therefore cheap insurance, not a hole.
- How to fix: either check that the stream has exactly one `token` event, equal to the line, and that it comes after
  the `action` (the fake streams an unreviewed line in several pieces, as the "Without review" test shows). Or record
  in the responder whether any token event had been pushed when the review call arrived. Then fix the comment.

### Minor (Nice to Have)

**M1. A stale code comment about the rollback clock.**

- Where: `src/turn/HostedSession.cpp:802`.
- What is wrong: the comment says "apply one when the time is before the latest live record". The rule is now the
  latest *seen* clock: the highest visible record, raised to the rollback not undone.
- How to fix: reword the comment as the header and the spec now say it.

**M2. Docs not in G3's documentation list are now stale.**

- Where:
  - `docs/superpowers/specs/2026-11-16-hosted-world-design.md:115` ("There is no review call") and `:246` ("Later: the
    optional review per turn and an explicit call tag");
  - `docs/superpowers/specs/2026-11-16-generation-controls-design.md:28` (hosted tags "`decide`, `resolve`,
    `speak`", now also `review` and `respeak`; `CallTag.h` cites this section);
  - `tests/hosted/fixtures/README.md:40-42`, the turn request's fields, without `review`. The main README points the
    host implementer at this file for "the request format".
- Plan gap: the G3 spec's Documentation section did not list these files.
- How to fix: one line each.

**M3. Plan Task 5 step 5 wraps not done.**

- Where: `tests/hosted/HostedDecideTests.cpp` (14 lines of 121-128 columns; the file is untouched in the range) and
  `src/turn/HostedSession.cpp:385` (the decide block, 125 columns).
- New lines over 120 columns in the range:
  - `src/server/Api.cpp:432` (123);
  - `src/turn/EngineText.h:120` (121);
  - `src/turn/Review.h:45` (121).
- How to fix: cosmetic, so it can wait.

**M4. A host `review` text that is too long fails at turn time, not at open.**

- Where: `src/turn/HostedSession.cpp:545`.
- What is wrong: the pack's `review` text has no budget check, which is what the spec says. A host text so long that
  the review prompt plus `review_max_tokens` and `review_thinking_tokens` exceeds the context makes every reviewed
  turn end with `error` 503 after its `action`, an `InferenceError` from the backend's context check. That is a
  data fault reported as "server unavailable".
- How to fix: if wanted, count the review text at `Open`, as the system text is counted. Unlikely in practice.

**M5. The problem limit in the hosted rules is not guarded.**

- Where: `src/turn/Review.cpp:17`.
- What is wrong: the `static_assert` guards REVIEW_RULES' "at most three" against REVIEW_MAX_PROBLEMS.
  HOSTED_REVIEW_RULES says "at most three" too, but the assert's message names only REVIEW_RULES.
- Related wording: `ParseReviewOutput`'s errors say "contradictions", which reads oddly in a hosted
  `timings.review.error` about speaking-rule problems.

**M6. The live review turn checks nothing review-specific.**

- Where: `tests/server/ServerLiveTests.cpp:91`.
- What is wrong: `timings.review` in `done` does not depend on the model's words, but it is not checked, and the
  turn's review cost is not printed (see I1).

**M7. The no-op resync test is only a guard.**

- Where: `tests/hosted/HostedRewindTests.cpp` ("A rewind that changes nothing...").
- What is wrong: the test would also pass before the change, because SyncHistory is incremental and nothing needs
  embedding. It is already noted in `progress.md`. Shares after an abandon (Task 4's minor) are still not tested;
  ends are.

**M8. Trivia.**

- `tests/hosted/HostedPromptPackTests.cpp:30`: "six texts it names (not decide)" should now say "(not decide or
  review)".
- `src/turn/HostedSession.cpp`: `turn/Review.h` is included before `turn/ResolveGrammar.h`, out of order.
- `PromptPack.cpp`: the MEMBERS column alignment (the Task 1 minor).
- The respeak with every problem dropped sends "Say it again without these problems:" followed by no list. This
  follows the spec, but the wording could be "Say it again within the speaking rules." in that case.

## Minor triage (progress.md "Final pass")

| Source | Minor | Status at head | Triage |
|---|---|---|---|
| Task 1 | PromptPack.cpp MEMBERS alignment | not changed | can wait |
| Task 1 | PromptPackTests three lines over 120 | wrapped | done |
| Task 1 | ReviewTests `none` line about 121 | wrapped | done |
| Task 2 | no RED run | n/a | can wait (the tests are specific enough to fail without the code) |
| Task 2 fix | review_max_tokens 384 in HostedSettings | done; settings.example.json is 384 too | done |
| Task 3 | server spec out-of-scope lists G3 | fixed | done |
| Task 3 | prompt-pack spec unknown-key list lacks review | fixed | done |
| Task 3 | README rules omit quotation marks | fixed | done |
| Task 3 | ApiReviewTests respeak-in-done | added | done |
| Task 3 | ApiReviewTests ordering | the test exists but does not test ordering | should fix (I2), small |
| Task 4 | rollback spec and HostedSession.h "latest live clock" | fixed in both | done; a code comment remains (M1), can wait |
| Task 4 | ends/shares after an abandon untested | end tested; share not | can wait |
| Task 4 | HostedSession.h:185 wrap | done | done |
| Task 4 | no-op resync test is a guard only | unchanged | can wait (M7) |
| Task 5 (plan step 5) | HostedDecideTests and decide-block wraps | not done | can wait (M3) |
| Controller | README `llama_server` typo | fixed in 854060f | done |

Nothing in the minor list must be fixed before merge. I1's docs part and I2 are the only items I would do first.

## Assessment

**Ready to merge?** With fixes

**Reasoning:** The code is correct and matches the plan and the specs. The G3 flow, cancellation, the commit of the
respoken line, the E2 clock rule and the Task 5 leftovers all check out, and Morpheus's own game is unaffected. Before
the integration summary goes to the Mantella implementer, the docs should state the review's real cost (I1: it
replaces the conversation slot's cached prompt, so the respeak and the next turn re-evaluate it), and the server
ordering test should test ordering (I2). Moving the review off the conversation's cache is a design choice for the
owner and does not block the merge.
