# M4 Task 4 report: SettingsEdit

Implemented app::SettingsEdit, ReadSettingsEdit, SaveSettingsEdit (src/app/model/SettingsEdit.h/.cpp) verbatim from the brief, plus tests/app/AppTestSupport.h (test::ReadFile, test::SettingsFolder) and tests/app/SettingsEditTests.cpp (4 test cases). Added model/SettingsEdit.cpp to src/app/CMakeLists.txt.

## TDD
RED: with the tests and CMake entry in place but no header:
`cmake --build --preset tests` -> `SettingsEditTests.cpp(10,1): error C1083: Cannot open include file: 'app/model/SettingsEdit.h'` (the configure step also failed: Cannot find source file model/SettingsEdit.cpp).
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit]"` -> All tests passed (22 assertions in 4 test cases).
Full: `ctest --preset tests` -> 100% tests passed out of 211.

## Files
src/app/CMakeLists.txt (M), src/app/model/SettingsEdit.h, src/app/model/SettingsEdit.cpp, tests/app/AppTestSupport.h, tests/app/SettingsEditTests.cpp.

## Self-review / concerns
Code is exactly the brief's. No warnings under /W4 /WX. No concerns.
