# Task 6 report: World loading

Status: DONE

## Implemented
- `src/core/World.h` / `World.cpp`: `MemoryMode`, `LoreEntry`, `World`, `LoadWorld`, transcribed verbatim from the brief.
- `src/core/CMakeLists.txt`: `World.cpp` added after `Dice.cpp`.
- `tests/core/WorldTests.cpp`: 3 test cases (valid world, content hash, 8 failure sections), verbatim.
- `tests/data/worlds/mini/`: world.json, rules.md, lore.csv, state_schema.json, initial_state.json, verbatim.

## TDD evidence
RED: `cmake --build --preset tests`
```
WorldTests.cpp(10,1): error C1083: Cannot open include file: 'core/World.h': No such file or directory
```
GREEN: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[world]"`
```
All tests passed (28 assertions in 3 test cases)
```
Full suite: `ctest --preset tests` -> 100% tests passed out of 40.

## Files changed
src/core/World.h, src/core/World.cpp, src/core/CMakeLists.txt, tests/core/WorldTests.cpp, tests/data/worlds/mini/* (5 files).

## Deviations
- None in code. Commit trailer uses `Co-Authored-By: Claude Sonnet 5.5` (per the attribution instruction in the session) rather than the `Opus 5.5` text in the brief's commit command.
- Git warned about LF->CRLF on the new files (autocrlf); the tests pass independent of this.

## Self-review
- Code compiled cleanly under /W4 /WX with no changes.
- Mini world data satisfies the stricter StateSchema rules (integer ranges, counter keys).
- Only the brief's paths were staged; untracked `dotnet/` left alone.

## Concerns
None.

## Fix round 1: out-of-range difficulty targets
Commit: 78b20f7 "Reject out-of-range difficulty targets"

### What changed
- `src/core/World.cpp`: the difficulties loop now range-checks each target before narrowing to `int`. Unsigned values are compared as `unsigned long long` (so huge values cannot wrap); signed values are read as `long long` and compared against `int` limits. Out-of-range values throw `DataError` with `"{worldWhere}: difficulty '{name}' must be an integer in int range"`. Added `#include <limits>`.
- `tests/core/WorldTests.cpp`: new SECTION "difficulty outside int range" in "LoadWorld reports the file at fault", using `{"hard": 4294967297}`.

### RED (before fix)
Command: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[world]"`
```
LoadWorld reports the file at fault
  difficulty outside int range
WorldTests.cpp(115): FAILED:
  CHECK_THROWS_WITH( LoadWorld(dir), ContainsSubstring("difficulty 'hard'") )
because no exception was thrown where one was expected:
test cases:  3 |  2 passed | 1 failed
assertions: 29 | 28 passed | 1 failed
```

### GREEN (after fix)
Command: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[world]"`
```
All tests passed (29 assertions in 3 test cases)
```
Full suite: `ctest --preset tests` -> 100% tests passed out of 40.

### Commands run
- `cmake --build --preset tests` (after adding the test, then after the fix)
- `./build/tests/bin/Debug/morpheus_tests.exe "[world]"` (before and after the fix)
- `ctest --preset tests`
- `git add src/core/World.cpp tests/core/WorldTests.cpp`
- `git commit -m "Reject out-of-range difficulty targets" -m "Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>"`

### Notes
- `dotnet/` was not staged. Git warned about LF->CRLF on the two edited files (autocrlf), as in the original task.
