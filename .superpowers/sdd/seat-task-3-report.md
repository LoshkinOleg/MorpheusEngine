# Task 3 report: ModelSeatBackend

Commit 73bc609 "Add ModelSeatBackend, which lets the model seat answer model calls". Files: src/app/model/ModelSeatBackend.h/.cpp, src/app/CMakeLists.txt, tests/app/ModelSeatBackendTests.cpp. Header, source and tests are the brief's code verbatim.

## TDD
RED: `cmake --build --preset tests` -> `tests\app\ModelSeatBackendTests.cpp(19,1): error C1083: Cannot open include file: 'app/model/ModelSeatBackend.h'`.
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[model_seat]"` -> All tests passed (3243 assertions in 17 test cases), run 3 times. `ctest --preset tests` -> 100% tests passed out of 303. No warnings.

## Deviation
The worker-level test ("Withdrawing a maintenance fold...") failed as written, deterministically: the first event of turn 4 was MemoryUpdated. Cause: the test posts turn 4 right after Withdraw, before the worker has left Maintain. A command waiting at that moment leaves maintenance due (Worker: `maintenanceDue = !_commands.empty()`), so it reruns ahead of the turn, requests the fold again (that is the `again` request), and emits MemoryUpdated. I added a 300 ms sleep after the Withdraw checks, with a comment, so the worker takes the withdrawal first.

## Concerns
- The sleep is timing-based (a loaded machine could in theory still race). The Worker has no idle signal to wait on; a deterministic fix would need one.
- The Await behaviour noted in the dispatch (Submit winning after a stop) is not special-cased in the brief's code; Complete treats it as a completed call.
