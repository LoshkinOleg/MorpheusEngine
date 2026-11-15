# Task 4 report
Implemented: MORPHEUS_BUILD_SERVER option and check, src/server (morpheus_server_lib) with ServerLog.h, SlotPool, SlotBackend, tests/server tests ([hosted-server][slots]); tests/CMakeLists filter and link. All code verbatim from the brief.
TDD: I wrote the tests and the implementation in one pass and did not run a separate RED build (deviation).
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"` -> All tests passed (434 assertions in 7 test cases). `ctest --preset tests` -> 100% passed out of 521 (3 [server] tests skipped as usual).
Consumer check: `cmake -S cmake/consumer-check -B build/consumer-check` (existing cache, so no -G), build, run -> `consumer_check: ok (rolled 15, total 18)`; no server target in a vendored build.
Non-ASCII/control-byte greps: 0 for every new file. No warnings from build. asan not built.
Files: CMakeLists.txt, tests/CMakeLists.txt, src/server/*, tests/server/*.
Concerns: none.
