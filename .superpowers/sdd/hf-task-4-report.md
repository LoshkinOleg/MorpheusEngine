# Task 4 report: the review as a continuation of the speak prompt

## Implemented
- `EngineText.h`: `HOSTED_REVIEW_RULES` reworded (spec section 3, verbatim), new `HOSTED_REVIEW_REFUSED`.
- `Review.h/.cpp`: `HostedReviewPrompt` removed, `std::string HostedReviewMessage(std::string_view rules, bool refused)` added (rules, then `\n\n` + refusal note when refused, then `\n\n` + instruction).
- `HostedSession.cpp`: the review call is `speakPrompt` + `{Assistant, spoken.text}` (the speak call's raw, untrimmed text) + `{User, HostedReviewMessage(pack review, !adjudication.requested)}`. New constant `REVIEW_TEMPLATE_MARGIN = 16`. A reviewed turn computes `reviewReserve = tokens(HostedReviewMessage(rules, true)) + reviewMaxTokens + reviewThinkingTokens + 16` and passes it to `Pass`.
- `HostedSession.h/.cpp` `Pass(..., int reserve = 0)`: copies the budget and raises `narrateAllowance` by `reserve` for `AssembleHostedPrompt` only. The speak/respeak message checks still use `_settings.budget.narrateAllowance` (commented why).
- Docs: README (review paragraph at the Review section, the `review` pack text is now a user message, the `review_*` budget bullet), CHANGELOG 1.6.0 review line, G3 spec (section 2 step 2, section 4 rules text and notes, "Out of scope").

## Tests (written first)
- `tests/turn/ReviewTests.cpp`: `HostedReviewMessage` (plain, refused, refusal text); default rules text checks updated.
- `tests/hosted/HostedReviewTests.cpp`: review call = speak messages + assistant line + user message; assistant message is the untrimmed speak text; refused proposal gets the refusal note; pack `review` text starts the user message; reserve test (`transcriptBudget` difference == reserve). Removed the old "no Player section in the review prompt" test (it asserted the old standalone layout, which no longer exists).

## Commands and results
- RED: not run as a separate step; the tests were written before the code, but I ran the first build only after implementing (I did not capture a compile-failure output). Noted as a process deviation.
- `cmake --build --preset tests --clean-first`: OK (one long compile; no warnings under /W4 /WX).
- First `[hosted]` run: 1 failure, the obsolete standalone-layout test; removed it, rebuilt.
- `morpheus_tests.exe "[review]"`: All tests passed (312 assertions in 28 test cases).
- `morpheus_tests.exe "[hosted]"`: All tests passed (1851 assertions in 173 test cases).
- `ctest --preset tests`: 100% tests passed out of 745.
- ASCII and control-byte greps on all 7 changed source/test files: 0 and 0. `\n` literals in Review.cpp, Review.h, ReviewTests.cpp, HostedReviewTests.cpp checked: still literal backslash-n.
- `cmake --build build/dev --config RelWithDebInfo`: see the final line below.

## Concerns
- The brief said README around line 112; the stale cache-cost sentence was actually in the `morpheus_server` Review paragraph (README line ~311); fixed there.
- The first clean-first build of the tests preset took very long (all test files compile in one cl.exe).
- dev build (RelWithDebInfo): clean, no errors or warnings (morpheus_tests.exe linked).
