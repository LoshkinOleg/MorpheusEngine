# Final pass, final fix round: requirements

The final review of the final pass is `.superpowers/sdd/fp-final-review.md` (read it for the detail). The controller
decided the items below (2026-11-16). Commits as you see fit (behaviour changes with their tests first, docs and wraps
separately). HEAD at the start: 854060f.

## 1. The review's cost on the prompt cache, documented (I1; controller: docs, no code change)

The review prompt is standalone and runs on the conversation's slot, so it replaces that slot's cached prompt: the
respeak (when there is one) and the conversation's next resolve call re-evaluate the whole conversation prompt. A
continuation of the speak prompt, or a slot of its own, would avoid this; that is a design question for the host, so
it stays open.

- README's Review paragraph and CHANGELOG 1.5.0: the cost is one review call, sometimes a respeak, and the
  conversation's prompt cache, which the review replaces (the next call of that conversation re-reads its whole
  prompt). Say to measure with that in mind.
- The G3 spec (`docs/superpowers/specs/2026-11-16-hosted-review-design.md`): section 2 step 2 gains a sentence saying
  the same; "Out of scope" gains "a review that keeps the conversation's prompt cache (a continuation of the speak
  prompt, or a slot of its own): a later decision".
- The live test (`tests/server/ServerLiveTests.cpp`, the review turn): check that `done`'s `timings` contain `review`
  (it does not depend on the model's words), and print (WARN, as the file prints first-sentence times) the review
  call's milliseconds and, for the turn after it if the fixture has one, the reused share of its prompt from its
  `timings.resolve` (only if the telemetry has `tokens_reused` and `prompt_tokens`; read `CallTelemetry` in
  `src/turn/Session.cpp` for the field names).

## 2. The server ordering test (I2)

`tests/server/ApiReviewTests.cpp`: the "action before tokens" check also passes without a review. Check instead that
the review turn's stream has exactly one `token` event, whose text is the whole line (the line is delivered in one
piece after the review), and keep the action-first check.

## 3. When no problem fits, the respeak uses the plain speak instruction (M8)

`src/turn/HostedSession.cpp`, the respeak message: when the problems were all dropped to fit `narrate_allowance`, use
the plain speak message (a correction that lists no problem says nothing). Update the G3 spec's section 2 step 4 and
the Global-Constraints-style comment in the code. Test in `tests/hosted/HostedReviewTests.cpp` (case 5's branches:
with no problem left the message equals the speak message).

## 4. Tests and comments

- `src/turn/HostedSession.cpp` (~802): the comment "apply one when the time is before the latest live record" says the
  latest seen clock (the highest visible record, raised to the most recent rollback not undone). Also put
  `turn/Review.h` after `turn/ResolveGrammar.h` in the includes if that is the file's order.
- `src/turn/Review.cpp` (~17): the `static_assert` that ties "at most three" to `REVIEW_MAX_PROBLEMS`: make its message
  (or a second assert) cover `HOSTED_REVIEW_RULES` too.
- `tests/hosted/HostedRewindTests.cpp`: a share made after an abandon is dated at the rollback's time (as the end test
  does, through a rewind that hides it only if so dated).
- `tests/hosted/HostedPromptPackTests.cpp` (~30): "six texts it names (not decide)" -> "(not decide or review)".

## 5. Stale docs (M2)

- `docs/superpowers/specs/2026-11-16-hosted-world-design.md` (~115 "There is no review call", ~246 "Later: the optional
  review per turn and an explicit call tag"): a hosted turn may ask for a review (G3, `2026-11-16-hosted-review-design.md`);
  the call tag is done (G2).
- `docs/superpowers/specs/2026-11-16-generation-controls-design.md` (~28): the hosted tags are `decide`, `resolve`,
  `speak`, and with a review `review` and `respeak` (G3).
- `tests/hosted/fixtures/README.md` (~40-42): the turn request's fields gain `review` (optional boolean; G3).

## 6. Wraps (M3), only lines you can wrap without changing code

`src/server/Api.cpp` (~432), `src/turn/EngineText.h` (~120), `src/turn/Review.h` (~45), `src/turn/HostedSession.cpp`
(~385), and the 121-128 column lines of `tests/hosted/HostedDecideTests.cpp`.

## Finally

`cmake --build --preset tests`, `"[review],[rewind],[hosted],[hosted-server]"`, `ctest --preset tests`, then
`cmake --build build/dev --config RelWithDebInfo` (every target; the live test is compiled there too). Foreground
only, never in the background. Byte checks on every changed file; watch for decoded "\n" escapes. Report per item
with commands and output.
