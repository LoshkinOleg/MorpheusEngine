# D1 Task 1 report: extended grammar::Validate subset

Status: DONE. Commit 4ed97c7 "grammar::Validate reads an extended subset for validation-only schemas; add CheckValidationSchema".

## Implemented
- `src/grammar/Schema.h`: new Validate comment (extended subset) and `CheckValidationSchema` declaration, verbatim from the brief.
- `src/grammar/Schema.cpp`: replaced whole file with the brief's code (Use::Grammar / Use::Validation, Number and Boolean nodes, optional keys, open objects, unbounded strings and arrays).
- `tests/grammar/ValidateExtendedTests.cpp`: new, brief's 7 test cases.
- `tests/grammar/SchemaTests.cpp`: `GrammarOnlyError` helper added; 11 `SchemaError(` calls swapped (lines 244, 245, 257, 265, 270, 311, 322, 329, 334, 340, 372 before the helper was inserted); `"boolean"` -> `"null"` in the "Validate throws the schema's DataError" test.

## TDD evidence
RED: `cmake --build --preset tests` failed to compile as expected:
`ValidateExtendedTests.cpp(184,5): error C2039: 'CheckValidationSchema': is not a member of 'morpheus::grammar'` (also C3861).
(An earlier error, `CHECK_THROWS_WITH` not found, was a missing include; see deviations.)

GREEN:
- `./build/tests/bin/Debug/morpheus_tests.exe "[schema]"`: `All tests passed (559 assertions in 40 test cases)`, no warnings.
- `ctest --preset tests`: `100% tests passed out of 434`; 3 `[server]` tests Not Run (Skipped).
- `ctest --preset asan` (after `cmake --build --preset asan`): `100% tests passed out of 434`; same 3 skipped. Only build warning: the existing LNK4300 /INCREMENTAL-with-ASAN linker note.

## Byte checks (Schema.h, Schema.cpp, ValidateExtendedTests.cpp, SchemaTests.cpp)
- `LC_ALL=C grep -c $'[\x80-\xFF]'`: 0 for all four files.
- `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'`: 0 for all four files.
- The code and tests contain no backslash-u / backslash-x escapes. The old header comment's `\\ud800` example is gone because the brief's replacement text does not contain it.

## Files changed
src/grammar/Schema.h, src/grammar/Schema.cpp, tests/grammar/ValidateExtendedTests.cpp (new), tests/grammar/SchemaTests.cpp.

## Deviations
- Added `#include <catch2/matchers/catch_matchers_string.hpp>` to ValidateExtendedTests.cpp: `CHECK_THROWS_WITH` is not visible through `catch_test_macros.hpp` alone (other test files include it the same way).

## Concerns
None. `.superpowers/sdd/progress.md` was already modified at start and was not touched or staged.
