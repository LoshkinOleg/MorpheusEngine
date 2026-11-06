# Task 2 Report: RFC 4180 CSV parser

## Status
DONE

## What was implemented
- `morpheus::core::ParseCsv(std::string_view)` in `src/core/Csv.h` and `src/core/Csv.cpp`, returning `std::vector<std::vector<std::string>>`.
- Supports quoted fields (commas, doubled quotes, embedded newlines), LF and CRLF record endings, a missing final newline, empty fields, and skipped blank lines.
- Throws `DataError` with a 1-based line number for an unterminated quote, a quote inside an unquoted field, text after a closing quote, or a bare CR.
- Registered `Csv.cpp` in `src/core/CMakeLists.txt`.
- Six Catch2 test cases in `tests/core/CsvTests.cpp`, tagged `[csv]`.

## TDD evidence

RED: `cmake --build --preset tests` after writing the test file only:
```
C:\Users\user\Desktop\MorpheusEngine\tests\core\CsvTests.cpp(7,1): error C1083: Cannot open include file: 'core/Csv.h': No such file or directory [...morpheus_tests.vcxproj]
```

GREEN: `cmake --build --preset tests` succeeded (BUILD_EXIT=0), then `./build/tests/bin/Debug/morpheus_tests.exe "[csv]"`:
```
Filters: [csv]
All tests passed (9 assertions in 6 test cases)
```

Full suite: `ctest --preset tests`:
```
100% tests passed out of 8
```
(2 hash tests + 6 csv tests.)

## Files changed
- Created: `src/core/Csv.h`, `src/core/Csv.cpp`, `tests/core/CsvTests.cpp`
- Modified: `src/core/CMakeLists.txt` (added `Csv.cpp`)

## Commit
- `96fb661 Add RFC 4180 CSV parser`

## Deviations from the brief
- Commit trailer: the brief specifies `Co-Authored-By: Claude Opus 5.5`. The attribution reminder in the session context says to use `Claude Haiku 5.5` for commits from here on, so I used Haiku 5.5. Change the trailer with `git commit --amend` if the brief's value is the one wanted.
- No code changes beyond the brief. It compiled under /W4 /WX on the first try and all tests passed on the first GREEN run.

## Self-review findings
- Code and tests were transcribed verbatim from the brief.
- Reviewed the state machine by hand against the test inputs, including the doubled-quote case `"say ""hi"""` and the `\r\n` blank-line case `\r\n` in `a\n\n\r\nb\n`. Both match the expected output.
- `std::size_t` is used with only `<string>`, `<string_view>` and `<vector>` included. It compiled cleanly under MSVC, so no change was made, but `<cstddef>` would make the include explicit if desired.
- `git` printed CRLF conversion warnings on commit (`LF will be replaced by CRLF`). These come from the repo's line-ending settings and do not affect the build or tests.

## Concerns
- None blocking. The CRLF warnings are informational and match the existing files in the repo.
