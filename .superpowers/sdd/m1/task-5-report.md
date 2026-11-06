# Task 5 Report: Deterministic dice

Status: DONE_WITH_CONCERNS (only the commit trailer, see Concerns)

## What was implemented

Deterministic dice and check resolution in `morpheus::core`:

- `SplitMix64`: 64-bit generator with the reference SplitMix64 constants. Output is identical across compilers and standard libraries.
- `DiceRule::Parse`: parses `NdS` and `NdS+stat` (case and spaces ignored). Throws `DataError` for malformed input, count outside 1..20, or sides outside 2..1000.
- `CheckSeed(runSeed, turn, checkIndex)`: derives a per-check seed from the run seed, turn, and check index. Throws `ContractError` via `MORPHEUS_REQUIRE` when turn < 1 or checkIndex < 0.
- `RollCheck(rule, statValue, target, seed)`: rolls `rule.count` dice, adds the stat when `rule.addStat` is set, and sets `success = total >= target`.

Because rolls depend only on `(runSeed, turn, checkIndex)`, regenerating a turn reproduces its dice.

## TDD evidence

RED: `cmake --build --preset tests` after adding only `tests/core/DiceTests.cpp`:

```
C:\Users\user\Desktop\MorpheusEngine\tests\core\DiceTests.cpp(5,1): error C1083: Cannot open include file: 'core/Dice.h': No such file or directory [C:\Users\user\Desktop\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
```

GREEN: after adding `Dice.h`, `Dice.cpp`, and the CMake entry:

```
$ cmake --build --preset tests          -> BUILD_EXIT=0 (no /W4 /WX warnings)
$ ./build/tests/bin/Debug/morpheus_tests.exe "[dice]"
Filters: [dice]
All tests passed (628 assertions in 6 test cases)
```

Full suite: `ctest --preset tests` -> `100% tests passed out of 37`, exit 0.

## Files changed

- Created `src/core/Dice.h`
- Created `src/core/Dice.cpp`
- Created `tests/core/DiceTests.cpp`
- Modified `src/core/CMakeLists.txt` (added `Dice.cpp` after `Changes.cpp`)

## Commit

`9fc0792 Add deterministic dice and check resolution`
Staged only `src/core` and `tests/core/DiceTests.cpp`, as the brief specifies. The untracked `dotnet/` folder was not touched.

## Deviations from the brief

None. All code was transcribed verbatim. No compile or test fixes were needed.

## Self-review findings

- Parse edge cases traced by hand against the test list: `""`, `"d20"`, `"1d"`, `"1x20"`, `"1d20+luck"`, and `"1d20+stat+stat"` all reach a `DataError` path. The `+stat` suffix is stripped once, so a doubled suffix fails number parsing.
- `from_chars` plus the full-segment check rejects trailing junk, so `"20+luck"` cannot parse as 20.
- `std::format` with `std::string_view` and `<format>` compile under `/std:c++20` with the installed MSVC toolset.
- No warnings under `/W4 /WX`.
- Modulo bias in `RollCheck` is noted in the code comment as acceptable for these die sizes.

## Concerns

1. Commit trailer: the brief's commit command specifies `Co-Authored-By: Claude Opus 5.5`. The session attribution guidance specifies `Claude Haiku 5.5`. I used the brief's trailer verbatim because the brief is the user's own instruction. If Haiku 5.5 should be credited, amend with `git commit --amend` before pushing.
2. Line endings: git printed `LF will be replaced by CRLF` for the four new or changed files. This is a warning about the working-copy line-ending setting, not a failure. The committed content is unaffected.
