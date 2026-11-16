# Task 3 report
Implemented: Api.cpp parses optional boolean `review` (400 "review: expected true or false"), done.timings gains review/respeak; API_VERSION 1.5.0; all "1.4.0" pins in tests/server -> "1.5.0"; new tests/server/ApiReviewTests.cpp (2 cases); live fixture case sends review:true on the last turn; README, CHANGELOG, server spec, prompt-pack spec updated.
TDD: deviation - implementation and tests were written together; the RED run was not captured. GREEN: `morpheus_tests.exe "[hosted-server]"` -> All tests passed (1375 assertions in 69 test cases); `ctest --preset tests` passed (live cases skipped).
ASCII/control-byte greps: 0 and 0 for ApiReviewTests.cpp, Api.cpp, ServerLiveTests.cpp.
Commit: see git log (one commit, review test file amended in).
