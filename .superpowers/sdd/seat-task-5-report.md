# Task 5 report: the Model seat window

Commit 958662b "Add the Model seat window".

## Implemented
ModelSeatWindow (new ModelSeatWindow.cpp), shared DrawPrompt (Ui.cpp, declared in Windows.h; ContextWindow now calls it), status-bar text "Waiting for the model seat: the <label> call", dock place on the right with State and Memory, MODEL_SEAT_WINDOW constant, CMake entry. Code taken verbatim from the brief. Five new [ui] tests in tests/app/UiTests.cpp (idle + checkbox, Submit Human, Draft Check+Submit, status bar/docked, Withdraw cancels turn).

## TDD
RED: `cmake --build --preset tests` ->
`UiTests.cpp(101,86): error C2039: 'ModelSeatWindow': is not a member of 'morpheus::app'` and `error C2039: 'MODEL_SEAT_WINDOW': is not a member of 'morpheus::app'` (plus follow-on C2065/C2061).

GREEN: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[ui]"` -> "All tests passed (246 assertions in 9 test cases)"; re-run 4 more times, all pass (assertion count varies by one or two between runs from frame timing only; no failures).
`ctest --preset tests` -> 100% tests passed out of 311 (no warnings).
App: `cmake --build build/dev --config RelWithDebInfo --target morpheus` built with no warnings; `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120` exit 0.

## Files changed
src/app/ui/Windows.h, Ui.h, Ui.cpp, ContextWindow.cpp, ModelSeatWindow.cpp (new), src/app/CMakeLists.txt, tests/app/UiTests.cpp.

## Deviations
None. The tests already synchronise on observable conditions (PumpUntil on Pending()/CanPost()), so nothing racy was changed. ContextWindow still has the public `showRaw` member, as in the brief.

## Concerns
None. Git warns about LF->CRLF on the committed files (repo autocrlf), harmless. `.gitmodules` stayed out of the commit.
