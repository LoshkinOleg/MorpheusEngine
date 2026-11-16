# Final fix round report

Commits: 1ebd557 (behaviour and tests), 469eab7 (docs, comments, wraps).

1. I1 docs: README Review paragraph, CHANGELOG 1.5.0, G3 spec section 2 step 2 and Out of scope state the prompt-cache cost.
   Live test (ServerLiveTests.cpp, review turn): REQUIRE timings.review, WARN review call ms (prompt, generate), consistent, respeak ran.
   The fixture ends with the review turn, so no "next turn reused share" is printed (not added; nothing to run it on).
2. I2: ApiReviewTests now requires exactly one token event equal to the whole line (kept the action-first check; removed unused includes).
3. M8: respeak uses the plain speak message when all problems were dropped (HostedSession.cpp, spec step 4, comment). RED: HostedReviewTests
   case 5 "fitting == speak" failed (message was the bare correction); GREEN after the change.
4. Comment at the rewind clock, include order, Review.cpp static_assert message (covers HOSTED_REVIEW_RULES; one assert, message only),
   new test "A share made after an abandon is dated at the rollback's time" (pending share clock == 1.5, a rewind to 1.4 hides it),
   HostedPromptPackTests wording.
5. Stale docs: hosted-world spec (two places), generation-controls spec, fixtures README (review field).
6. Wraps: Api.cpp, EngineText.h, Review.h, HostedSession.cpp decide block, HostedDecideTests (all lines now <= 120).

Verification: `cmake --build --preset tests` clean; `"[review],[rewind],[hosted],[hosted-server]"`: 246 cases, 245 passed, 1 skipped, 3326 assertions;
`ctest --preset tests`: 100% passed out of 728; `cmake --build build/dev --config RelWithDebInfo` finished with no errors.
Byte checks (non-ASCII and control bytes) print 0 for every changed file; no decoded escapes.
Concern: the new share test was not run against the pre-E2 code to see it fail. Nothing left running.
