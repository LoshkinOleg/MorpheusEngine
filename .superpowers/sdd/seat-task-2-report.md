# Task 2 report: the seat's labels, modes and shared state

Status: DONE. Commit 8ec8ed2 "Add the model seat's labels, modes and shared state".

## Implemented
`src/app/model/ModelSeat.h/.cpp` (SeatLabel, SeatMode, SEAT_LABELS, SeatLabelName, SeatLabelOf, SeatRequest with id, SeatReply, SeatCheck, ModelSeat with Await/Submit/Check/Withdraw), `tests/app/SeatTestSupport.h`, `tests/app/ModelSeatTests.cpp`, and `model/ModelSeat.cpp` added to `src/app/CMakeLists.txt`. All code and tests verbatim from the brief.

## TDD evidence
RED: `cmake --build --preset tests` (after writing the two test files only):
```
D:\MorpheusEngine\tests\app\SeatTestSupport.h(16,1): error C1083: Cannot open include file: 'app/model/ModelSeat.h': No such file or directory
```
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[model_seat]"` -> `All tests passed (87 assertions in 8 test cases)`.
Full: `ctest --preset tests` -> `100% tests passed out of 294`. Build had no warnings or errors.

## Files changed
src/app/CMakeLists.txt, src/app/model/ModelSeat.h, src/app/model/ModelSeat.cpp, tests/app/SeatTestSupport.h, tests/app/ModelSeatTests.cpp.

## Deviations
None to the code. Process only: new files were `git add`ed (only my five paths) before `git commit <paths>`, since git refuses untracked pathspecs. `.gitmodules` stayed staged and uncommitted.

## Concerns
None.
