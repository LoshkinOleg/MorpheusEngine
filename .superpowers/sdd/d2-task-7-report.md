# Task 7 report
Implemented server::RunRegistry (header, cpp, CMake entry, tests) verbatim from the brief.
TDD: the RED step (missing header compile error) was not run separately; files were written together. GREEN: `morpheus_tests.exe "[registry]"` -> All tests passed (41 assertions in 5 test cases). Full ctest --preset tests: no failures (llama/server tests skipped).
ASCII/control-byte greps: 0 and 0 for all three files.
Deviations: none. Concerns: none.

## Fix round
1. Open refuses an id equal to an opened run's id ignoring ASCII case but not exactly (DataError naming both ids); documented on Open and CheckRunId. Test: open "save_1", Open("SAVE_1") throws, Find("SAVE_1") is nullptr, exact id still reopens.
2. StopAll sets `_stopping` under `_mutex`; Open then throws InferenceError "the server is stopping" and registers nothing. Test added.
Tests were written first (not run RED separately). Commands:
- `./build/tests/bin/Debug/morpheus_tests.exe "[registry]"` -> All tests passed (49 assertions in 7 test cases)
- `./build/tests/bin/Debug/morpheus_tests.exe "[worker]"` -> All tests passed (202 assertions in 18 test cases)
Build: no warnings. Non-ASCII greps: 0 for all three files.

### Fix round 2
- Open rechecks `_stopping` under the final `_mutex` just before `emplace`; if set, the idle worker is dropped (its destructor stops and joins it) and InferenceError "the server is stopping" is thrown. Why the race is closed: StopAll and the recheck+emplace both run under `_mutex`, so either StopAll runs first (the recheck sees the flag and the worker is never registered) or the emplace runs first (StopAll then sees and stops the worker). No third ordering exists. No test: the window is not deterministically reachable.
- `_mutex` comment is now "Guards _runs and _stopping"; the case note was removed from CheckRunId's comment.
- `./build/tests/bin/Debug/morpheus_tests.exe "[registry]"` -> All tests passed (49 assertions in 7 test cases)
