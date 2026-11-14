# Task 4 report: turn::HostedWorld
Implemented verbatim from the brief (files extracted from the brief with sed, so bytes are exact).
RED: with src/turn/HostedWorld.* moved aside and the CMake change stashed, `cmake --build --preset tests` failed: `tests\hosted\HostedTestSupport.h(13,1): error C1083: Cannot open include file: 'turn/HostedWorld.h'`.
GREEN: `[hosted]` -> All tests passed (299 assertions in 23 test cases). `ctest --preset tests` and `ctest --preset asan`: 100% passed out of 464 (3 [server] skipped). 464 = 453 + 11.
Byte checks: both greps printed 0 for all 7 files.
Files: src/turn/HostedWorld.h/.cpp, src/turn/CMakeLists.txt, tests/hosted/{HostedTestSupport.h,HostedWorldTests.cpp,fixtures/open.json,fixtures/observation.json}.
Deviations: none. Commit trailer names Sonnet 5.5. No warnings besides the usual ASAN LNK4300.

## Fix round 1
1. Adjudicate now catches nlohmann::json::exception (was parse_error). Test "Adjudicate refuses a number the JSON parser cannot hold" (1e999).
2. PointerToken declared once in grammar/Schema.h, defined in Schema.cpp (outside the anonymous namespace); HostedWorld.cpp's copy removed and calls grammar::PointerToken.
3. Empty allowed list: DataError "allowed_actions: the list names no action", checked at the top of ProposalSchema.
4. Test that minLength reaches the proposal schema.
RED: `cmake --build --preset tests` then `morpheus_tests.exe "[hosted]"`: 25 cases, 2 failed, 3 assertions failed (1e999 threw; empty list message). The minLength check passed at once (coverage only).
GREEN: `[hosted]` 305 assertions in 25 cases passed; `[grammar]` passed; `ctest --preset tests` and `ctest --preset asan`: 100% passed out of 466. Byte greps: 0 for all 4 files. No processes left.
