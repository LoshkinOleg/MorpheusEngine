# M5 final fixes
1. TurnLogWindow: "digest only" when no verdict and no error; "no verdict" only with an error.
2. World.h: "Everything in one folder" comment moved above struct World.
3. Review.cpp: reason now "'digest' is longer than {} characters" (no test matched the old text).
4. SettingsEdit.h: comment re-wrapped to 120 columns.
5. Review.cpp: static_assert(REVIEW_MAX_PROBLEMS == 3, ...) with message.
6. Replay.cpp: MORPHEUS_REQUIRE the review grammar has a newline.
7. Settings.cpp: DataError when review_max_tokens + review_thinking_tokens >= context_size. Test first in SettingsTests.cpp: RED (no exception thrown, 2 assertions failed), GREEN after the check.
Verification: [review],[replay],[world],[settings],[ui],[inspect]: all passed (1163 assertions, 47 cases). ctest --preset tests: 100% passed out of 285. build/dev morpheus target built.
