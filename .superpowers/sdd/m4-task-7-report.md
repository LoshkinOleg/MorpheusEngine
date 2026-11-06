# Task 7 report: EngineHost

Status: DONE

## Implemented
- src/app/model/EngineHost.h/.cpp verbatim from the brief (background loader jthread, mutex hand-over, Poll, backend kept when only sampling/memory mode change).
- tests/app/EngineHostTests.cpp (4 test cases, tag [engine_host]) verbatim from the brief.
- tests/app/AppTestSupport.h: added test::EngineSettings() and the core/Settings.h include.
- src/app/CMakeLists.txt: added model/EngineHost.cpp.

## TDD
- RED: `cmake --preset tests && cmake --build --preset tests` with the test file and CMake entry present but no header:
  `EngineHostTests.cpp(14,1): error C1083: Cannot open include file: 'app/model/EngineHost.h'` (CMake also reported "Cannot find source file: model/EngineHost.cpp" before the file existed).
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[engine_host]"` -> `All tests passed (31 assertions in 4 test cases)`.

## Other runs
- `[engine_host]` 8 more times in the tests preset (Debug): all passed every time, no flakiness.
- `ctest --preset tests`: 100% passed, 227/227.
- asan preset (configured and built, only the usual LNK4300 incremental warning): `[engine_host]` run 4 times directly plus `ctest --preset asan -R EngineHost` (4/4): all passed, no ASan reports.

## Files changed
src/app/CMakeLists.txt, src/app/model/EngineHost.h, src/app/model/EngineHost.cpp, tests/app/AppTestSupport.h, tests/app/EngineHostTests.cpp. Nothing else staged.

## Self-review
- Member order: _loader last (joined first), _pWorker after _pBackend (destroyed first). Matches the threading constraint.
- Start after Failed reloads (no backend, so sameBackend is false); _loader was already joined in Poll, so reassigning the jthread is safe.
- Destroying a host while a load is in flight blocks until the factory returns (jthread join); acceptable, loads cannot be interrupted.

## Concerns
None.
