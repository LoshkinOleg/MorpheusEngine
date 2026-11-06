# M4 Task 3 report: catalog of worlds and runs

Status: DONE. Commit 9a24e64 "List worlds and runs for the app".

## Implemented
src/app module with `morpheus_app_model` (links morpheus_turn); `app/model/Catalog.{h,cpp}` exactly as in the brief
(ListWorlds, ListRuns, RunFile, RunNameProblem, DefaultRunName, ParseSeed, LocalTimeText, RUN_NAME_MAX_CHARS and the
three structs). Root CMakeLists adds `src/app` when MORPHEUS_BUILD_APP or MORPHEUS_BUILD_TESTS; tests link
`morpheus_app_model`. No deviations; chrono (clock_cast, zoned_time, current_zone) compiled under MSVC as written.

## TDD evidence
RED: with the tests, CMake edits and src/app/CMakeLists.txt in place but no Catalog sources:
`cmake --preset tests` -> "CMake Error at src/app/CMakeLists.txt:2 (add_library): Cannot find source file: model/Catalog.cpp"
(fails at configure, before reaching the missing-header compile error the brief predicted; same cause).
GREEN: `cmake --preset tests && cmake --build --preset tests` clean (no warnings/errors);
`./build/tests/bin/Debug/morpheus_tests.exe "[catalog]"` -> "All tests passed (29 assertions in 5 test cases)".
`ctest --preset tests` -> 100% tests passed out of 207.

## Files
CMakeLists.txt, tests/CMakeLists.txt, src/app/CMakeLists.txt, src/app/model/Catalog.h, src/app/model/Catalog.cpp,
tests/app/CatalogTests.cpp. Nothing else staged.

## Self-review / concerns
None blocking. Notes: git warns LF->CRLF on the new files (autocrlf, benign). src/app/ui/ImGuiConfig.h (earlier task)
already existed under src/app; src/app/CMakeLists.txt does not reference it. ListRuns has no test for a lastWrite tie
(name ordering) - brief did not require it.
