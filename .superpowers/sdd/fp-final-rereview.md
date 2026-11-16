# Re-review: final pass fix round, 854060f..469eab7

Reviewer: Opus, read-only (no builds, no tests).

What I read:
- the range package `review-854060f..469eab7.diff`;
- the fix brief `fp-final-fix-brief.md`;
- the report `fp-final-fix-report.md`;
- my findings in `fp-final-review.md`.

Outside the diff I read only:
- `tests/hosted/HostedFixtureTests.cpp`, to see how the fixture runner builds a request;
- `HostedTestSupport.h`, for `BELETHOR`;
- a line-length scan of the touched files.

## Findings, one by one

| Finding | Decision | Status | Notes |
|---|---|---|---|
| I1: the review replaces the conversation slot's cached prompt | docs only; a cache-keeping review is left open | **Resolved** | See "I1" below. |
| I2: the server ordering test | fix the test | **Resolved** | See "I2" below. |
| M8: respeak with every problem dropped | plain speak instruction | **Resolved** | See "M8" below. |
| M1: stale Rewind comment | reword | **Resolved** | `HostedSession.cpp:797-798` now says "latest seen clock (the highest visible record, raised to the most recent rollback not undone)". |
| M2: stale docs | fix each | **Resolved** | See "M2" below. |
| M3: wraps | wrap | **Resolved, except two leftovers** | See "M3 leftovers" below. |
| M5: static_assert covers HOSTED_REVIEW_RULES | message | **Resolved** | `Review.cpp:16-19`. The wording "contradictions" in `ParseReviewOutput` was not in the brief; it can wait. |
| M6: live check of `timings.review` | REQUIRE and WARN | **Resolved** | See "M6" below. |
| M7 / Task 4: share after an abandon untested | add a test | **Resolved** | See "Share after an abandon" below. |
| M8 trivia: include order, HostedPromptPackTests comment | fix | **Resolved** | |
| M4: host review text over the context | not in the brief | Open, can wait | Unchanged. |

### I1

- README Review paragraph (line 307), CHANGELOG 1.5.0 and G3 spec section 2 step 2 now say what the review costs:
  - the review prompt is standalone and runs on the conversation's slot;
  - so it replaces that slot's cached prompt;
  - so the respeak and the conversation's next call re-read the whole prompt.
- The README also tells the host where to look: "the next turn's `timings.resolve`, not only `timings.review`".
- The spec's "Out of scope" now records the open design question:
  - a continuation of the speak prompt, or
  - a slot of its own.
- That matches what the code does: `reviewSpec.slot` stays -1, so SlotBackend uses the conversation's slot.

### I2

- `ApiReviewTests.cpp` now requires exactly one `token` event, equal to the whole line, and keeps the action-first
  check.
- This now tells a reviewed turn from an unreviewed one. In the fake, an unreviewed line streams in several pieces;
  "Without review" checks `pieces > 1` on the same responder.
- The unused includes are gone.

### M8

- `HostedSession.cpp`: `if (problems.empty()) message = speakMessage;` replaces the second token count.
- This is correct because of two facts:
  - The loop stops either with at least one problem that fits, or with none left.
  - The speak message alone always fits (`checkSpeaker`).
- It is also correct because an inconsistent review always has 1-3 problems (grammar and parser). So
  `problems.empty()` can only mean every problem was dropped. It cannot mean a review that listed none.
- The comment and spec step 4 were updated.
- The test (`fitting == speak`) went RED before the change and GREEN after it, per the report.

### M2

- `hosted-world-design.md`, lines 115 and 246.
- `generation-controls-design.md`, line 28: the hosted tags now include `review` and `respeak`.
- `tests/hosted/fixtures/README.md`: the turn's fields now list `review`.

### M3 leftovers

- `src/turn/Review.h:46`: the re-wrapped comment line is still 121 columns.
- New lines over 120 columns:
  - `tests/hosted/HostedRewindTests.cpp:308` (121);
  - `CHANGELOG.md:15` (155; the rest of the file wraps at about 120).

### M6

- `ServerLiveTests.cpp`: the review turn REQUIREs `timings.review`.
- It WARNs:
  - the review call's milliseconds (prompt and generate);
  - its verdict;
  - whether a respeak ran.
- The fixture ends with the review turn, so the next turn's reused share cannot be printed. The report says so, and the
  code comment says why. Acceptable.
- The `.get<double>()` calls on the `*_ms` fields read integer JSON fine.

### Share after an abandon

- New test: "A share made after an abandon is dated at the rollback's time".
- It checks:
  - the pending share's clock is 1.5, not 1.0;
  - a rewind to 1.4 then hides it.
- That second check only holds if the share is dated 1.5, so the test proves the rule. It does not merely sit beside
  it.
- It reads the store through a second `RunStore::Open` on the same file, which is fine for SQLite reads.
- The report notes the test was never run against the code from before E2 (no RED run). It can wait: the rewind
  assertion depends on the dating.

## New minor finding

**N1. The fixtures README now lists `review` as a turn field the fixture runners do not use.**

- Where: `tests/hosted/fixtures/README.md:42`.
- What is wrong:
  - The README lists `review` among a fixture turn's fields. But the in-process fixture runner (`HostedFixtureTests.cpp`
    builds the request from `player_text`, `events`, `allowed_actions` and `observation`) ignores it.
  - The `model` block has no reply for a review call.
  - So a fixture author who sets `review: true` would get a turn without a review in-process. Over HTTP the field
    would pass through to the server.
- Why it matters: little. The file doubles as the host's request reference, and as such the line is right.
- How to fix (can wait), either:
  - add "(a host request field; the fixtures here do not use it)"; or
  - teach the runner `review` and a `model.review` reply.

## Assessment

**Ready to merge?** Yes

**Reasoning:** Every Critical and Important finding is resolved. I1 is resolved as decided: documented in the README,
CHANGELOG and spec, with the design left open. I2's test now tells a reviewed turn from an unreviewed one. The one
behaviour change (M8) is correct, and its test went RED before it. What remains is cosmetic or can wait: N1, three
lines over 120 columns, M4, the "contradictions" wording, and the share test's missing RED run.
