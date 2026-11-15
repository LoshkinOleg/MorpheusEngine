# Task 7 report
Status: DONE. Commit b0d63d6.
Implemented: two routes in src/server/Api.cpp, API_VERSION 1.1.0, CHANGELOG entry, "1.0.0" -> "1.1.0" in tests/server, new ApiMemoryTests.cpp and HttpMemoryTests.cpp (fixture replayed over real HTTP, its steps as they are now).
Follow-up: ApiTurnTests.cpp FoldedThrough replaced by HuldaRemembersC1 (memories route); the second use waits with WaitUntil.
TDD: RED was run by reverting Api.cpp only (routes): 5 test cases failed ("key 'memories' not found" etc). GREEN: "[hosted-server]" x3 -> All tests passed (1153 assertions in 57 test cases) each; asan same; ctest --preset tests 100% passed out of 602 (4 llama-server tests skipped).
Deviations: two dangling range-for/items() on temporaries in my test code (brief's HttpMemoryTests items() of a value() temporary caused a SIGSEGV; fixed with locals); same fix in the new helper. The heredoc write failed once, files written with Write tool instead.
Checks: non-ASCII and control-byte greps print 0 for all changed files. No morpheus/ctest processes left.
Concerns: none.
