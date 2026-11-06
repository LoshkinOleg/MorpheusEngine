# Task 8 report: AppController

Status: DONE

## Implemented
AppController and AppPaths (src/app/model/AppController.h/.cpp) exactly as the brief lays out (header and implementation verbatim). Test support PathsIn, ScriptedFactory and PumpUntil added to tests/app/AppTestSupport.h (with the extra includes). Five tests in tests/app/AppControllerTests.cpp, verbatim from the brief. model/AppController.cpp added to src/app/CMakeLists.txt.

## TDD evidence
RED: `cmake --preset tests; cmake --build --preset tests` with the test support, tests and CMake entry present but no controller files:
- `CMake Error at src/app/CMakeLists.txt:2 (add_library): Cannot find source file: model/AppController.cpp`
- `AppTestSupport.h(19,1): error C1083: Cannot open include file: 'app/model/AppController.h'`

GREEN: after adding header and implementation, `cmake --build --preset tests` had no errors or warnings, and
`./build/tests/bin/Debug/morpheus_tests.exe "[app_controller]"` -> `All tests passed (111 assertions in 5 test cases)`.
`ctest --preset tests` -> `100% tests passed out of 232`.
ASan: `cmake --preset asan`, build, `ctest --preset asan -R` for the five new tests -> 5/5 passed, no ASan reports.

## Repeat runs
`[app_controller]` run 13 times in total (3, then 10): all passed, no flakiness. The assertion count varies between 110 and 111 (PumpUntil's REQUIRE is counted per loop iteration, so it depends on timing); it is not a failure.

## Files changed (commit 5b89d32)
src/app/CMakeLists.txt, src/app/model/AppController.h, src/app/model/AppController.cpp, tests/app/AppTestSupport.h, tests/app/AppControllerTests.cpp

## Self-review
- Code matches the brief verbatim; naming, comment style and ASCII-only comply with CLAUDE.md.
- Nothing outside the brief's file list staged.
- Git prints LF->CRLF warnings on add (autocrlf); no content issue.

## Concerns
None.
