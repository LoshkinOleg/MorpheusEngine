# Task 3 report: Settings (settings.json loader)

Status: DONE
Commit: 82d2376 "Add settings.json loading" (branch `cpp`)

## What was implemented

- `src/core/Settings.h`: `SamplingSettings`, `BudgetSettings`, `Settings` and `LoadSettings(const std::filesystem::path&)`, in namespace `morpheus::core`, as the brief specifies.
- `src/core/Settings.cpp`: strict loader, verbatim from the brief. Unknown keys at every level are `DataError`s naming the key. Model files are resolved against the settings file's folder and must exist. Every integer and number is range-checked through `JsonIntInRange`. The budget must leave room for the transcript. `top_p` must be greater than 0.
- `tests/core/SettingsTests.cpp`: the two `[settings]` test cases from the brief, verbatim (1 happy path, 6 failure sections). Each failure section uses its own temp folder via `FreshTempDir`.
- `src/core/CMakeLists.txt`: added `Settings.cpp`.
- `settings.example.json` (repo root): the example document from the brief.
- `.gitignore`: appended `models/` and `settings.json`.

## TDD evidence

RED: `cmake --build --preset tests` after writing only the test file. Output:

```
C:\Users\user\Desktop\MorpheusEngine\tests\core\SettingsTests.cpp(11,1): error C1083: Cannot open include file: 'core/Settings.h': No such file or directory [C:\Users\user\Desktop\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
```

GREEN:
- `cmake --build --preset tests`: exit 0, no warnings or errors.
- `./build/tests/bin/Debug/morpheus_tests.exe "[settings]"`: `All tests passed (15 assertions in 2 test cases)`.
- `ctest --preset tests` (full suite): `100% tests passed out of 86`, exit 0.

## Files changed

- `src/core/Settings.h` (new)
- `src/core/Settings.cpp` (new)
- `tests/core/SettingsTests.cpp` (new)
- `src/core/CMakeLists.txt` (modified: one line)
- `settings.example.json` (new)
- `.gitignore` (modified: two lines, appended with CRLF to match the file)

Not touched: the root `settings.json`. It was never read, written, staged or deleted. `git status` shows it stays git-ignored, via `.git/info/exclude`.

## Deviations from the brief

- `Settings.cpp` sits after `JsonInt.cpp` and before `TurnRecord.cpp` in `src/core/CMakeLists.txt`. The brief said "after JsonInt.cpp", and `TurnRecord.cpp` was already in that position from an earlier task. Keeping the list sorted by task order, `Settings.cpp` goes between them.
- No other deviations. Code, test and example JSON are verbatim from the brief.

## Concerns

- Git prints `warning: in the working copy of ..., LF will be replaced by CRLF the next time Git touches it` for the new and modified text files. The working copies are LF, and git's line-ending conversion is configured to write CRLF. This is a repo-wide config matter and not a Task 3 issue. The commit is unaffected.
- `models/` and `settings.json` were already excluded locally via `.git/info/exclude`. The new `.gitignore` entries are the committed rule the brief asks for, so they do not change local behaviour.
