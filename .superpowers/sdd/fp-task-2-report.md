# Task 2 report: RunTurn reviews the line when asked

Commit 933ce6e "Hosted review: a turn asked to review holds its line back, reviews it, respeaks once on problems".

## Implemented
- `HostedTurnRequest::review` (default false) and RunTurn's comment.
- `RunTurn`: the speak call streams only without review; step 3b (review call, one respeak, delivery of the line in one piece); `record.line = line`; `timings.review`, `timings.respeak`. Code as in the brief.
- `HostedScript` answers calls tagged `review` from `reviews` (`reviewed` counter).
- `tests/hosted/HostedReviewTests.cpp`: the seven cases, tag `[hosted][review]`.

## TDD evidence
Honest note: the RED run (compile error, `review` not a member) was not captured. I wrote the implementation before the first build, so the first build was already the GREEN one (clean rebuild).
- GREEN `cmake --build --preset tests --clean-first`: no errors or warnings.
- `./build/tests/bin/Debug/morpheus_tests.exe "[review]"`: All tests passed (253 assertions in 21 test cases) (includes Task 1's review tests).
- `"[hosted]"`: All tests passed (1717 assertions in 157 test cases).
- `ctest --preset tests`: 100% tests passed out of 716 (the [llama]/[server] ones skipped).

## Escape / byte checks
- Non-ASCII grep: 0 for HostedReviewTests.cpp, HostedSession.cpp, HostedSession.h, HostedTestSupport.h.
- Control-byte grep: 0 for the same four files.
- The edit tool decoded `"\n"` in HostedSession.cpp once into real newlines; I repaired it, and `+ "\n"` (three places) are the literal escapes now (verified by reading the lines).
- Test file written with the Write tool; its `"\n"` strings are literal (the respeak-message test passes against them).

## Files changed
src/turn/HostedSession.h, src/turn/HostedSession.cpp, tests/hosted/HostedTestSupport.h, tests/hosted/HostedReviewTests.cpp (new).

## Deviations
- `test::HostedSettings()` did not set `review_max_tokens` (0, which the fake backend rejects as maxTokens must be positive). Added `settings.budget.reviewMaxTokens = 96` and updated its comment.
- Case 5: two sub-cases with the fake counts computed in the test: allowance = tokens(bare correction + speak) + 1 gives the bare correction message (both long problems dropped); allowance = tokens(speak) gives the plain speak message.
- Case 2 compares the review prompt's messages (role, content) and stableCount, since Prompt has no operator==.
- Added includes `turn/Review.h` and `turn/ResolveGrammar.h` to HostedSession.cpp.

## Concerns
None.

## Fix round
Commit 377c844 "Hosted review fix round: review_max_tokens 384 in the hosted test settings, more review tests, wraps".
- HostedSettings: reviewMaxTokens 384 (comment updated). Speak Complete(...) call and the HostedScript comment wrapped to 120.
- New tests: stop during the respeak; no player text (no "# Player" section, starts with "# Speaker\nHulda\n\n# Requested action\n"); case 5 partial set (first problem kept, second dropped); case 2 checks thinkingMaxTokens and the review seed; case 3 checks the respeak's maxTokens and Renarrate seed.
- Not RED-first for these: the implementation already behaved as asserted (they are added coverage).
- Byte checks: non-ASCII 0 and control-byte 0 for the three files. The edit tool decoded "\n" escapes in the new tests once; repaired and verified literal.
- `cmake --build --preset tests`: no errors/warnings. `"[review]"`: All tests passed (271 assertions in 23 test cases). `"[hosted]"`: All tests passed (1735 assertions in 159 test cases). `ctest --preset tests`: 100% tests passed out of 718.
