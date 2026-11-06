# Task 9 report: UI shell, Runs window, Play window

Status: DONE

## Implemented
Created src/app/ui/{Windows.h,Ui.h,Ui.cpp,RunsWindow.cpp,PlayWindow.cpp} and tests/app/UiTests.cpp verbatim from the brief.
Added `morpheus_app_ui` to src/app/CMakeLists.txt and linked it into morpheus_tests (tests/CMakeLists.txt).

## TDD
RED (tests + CMake in place, src/app/ui/Ui.h temporarily moved away):
`cmake --build --preset tests` -> `src\app\ui\Ui.cpp(1,1): error C1083: Cannot open include file: 'app/ui/Ui.h'`
GREEN: `cmake --build --preset tests` clean (no warnings), then
`./build/tests/bin/Debug/morpheus_tests.exe "[ui]"` -> All tests passed (128 assertions in 2 test cases).

## Results
- Repeat runs of [ui] (5 total): all pass (127 or 128 assertions; the count varies with timing because a CHECK sits in a poll loop's path, not a flake).
- ctest --preset tests: 100% passed, 234/234.
- No ImGui assertion failures; no API substitutions needed (all brief ImGui calls compile against v1.92.8-docking).

## Files changed
src/app/CMakeLists.txt, tests/CMakeLists.txt, src/app/ui/Windows.h, Ui.h, Ui.cpp, RunsWindow.cpp, PlayWindow.cpp, tests/app/UiTests.cpp.

## Self-review / concerns
- Code is as given in the brief; no deviations. asan preset not run.
- Headless tests verify no ImGui rule breaks and the model state, not pixel output or actual docking placement.
