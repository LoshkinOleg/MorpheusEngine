# Task 5 report: RunModel

Status: DONE

## Implemented
`app::RunModel` (src/app/model/RunModel.h/.cpp), code verbatim from the brief; `model/RunModel.cpp` added to `morpheus_app_model`; tests in tests/app/RunModelTests.cpp (7 test cases, tag [run_model]).

## TDD evidence
RED (header and cpp moved aside, then `cmake --build --preset tests`):
`tests\app\RunModelTests.cpp(10,1): error C1083: Cannot open include file: 'app/model/RunModel.h': No such file or directory`
GREEN: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[run_model]"` -> `All tests passed (51 assertions in 7 test cases)`.
`ctest --preset tests` -> 100% tests passed out of 218.

## Files changed
src/app/CMakeLists.txt, src/app/model/RunModel.h, src/app/model/RunModel.cpp, tests/app/RunModelTests.cpp

## Self-review / concerns
Built warning-free under /W4 /WX. No deviations from the brief. No concerns.
