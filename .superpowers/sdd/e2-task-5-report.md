# Task 5 report
Implemented: POST /v1/runs/{run_id}/rewind (Api.cpp), API_VERSION 1.2.0 (all "1.1.0" in tests/server updated), CHANGELOG entry, PlayMemoryFixtureOverHttp steps (clock in open body, step clocks, rewind, turns, conversation, events), ApiRewindTests.cpp, reload fixture over HTTP test.
TDD: the RED run was skipped. A failed shell heredoc forced me to redo the edits and I wrote the route before the first build, so the tests were first seen passing. Deviation noted.
GREEN: `[hosted-server]` 3 runs on tests: 1253 assertions in 61 test cases passed each time. asan `[hosted-server]`: same, passed. `ctest --preset tests`: 100% passed of 646 (5 llama-server tests skipped as expected).
Byte checks on all 6 changed files: 0 and 0 each. No processes left running.
Files: src/server/Api.h, src/server/Api.cpp, CHANGELOG.md, tests/server/{ApiRewindTests.cpp,HttpMemoryTests.cpp,ServerTestSupport.h and the files with "1.1.0"}.
Concerns: none.
