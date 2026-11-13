# Task 7 report: Validate

Implemented grammar::ValidationError and grammar::Validate in src/grammar/Schema.h/.cpp exactly as the brief's code, reusing Task 6's Compile/Node/PointerToken (Validate calls Compile, so it throws FromSchema's DataError, including the maxLength/maxItems limit errors). Tests (SchemaError now checks Validate too, OnlyError, 6 new cases) added to tests/grammar/SchemaTests.cpp.

## TDD
RED: `cmake --build --preset tests` -> SchemaTests.cpp(48): C2039 'Validate': is not a member of 'morpheus::grammar'; C3861 identifier not found; C2039 'ValidationError' not a member; C4430 ...
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[schema]"` -> All tests passed (449 assertions in 32 test cases). Build: no warnings.
Full: `ctest --preset tests` -> 100% tests passed out of 364.

## Escape checks
Byte scan (python) of the three files: 0 non-ASCII bytes, 0 control bytes, no decoded escapes (the literal backslash-x/u in the test file are pre-existing and my added text uses none).

## Files
src/grammar/Schema.h, src/grammar/Schema.cpp, tests/grammar/SchemaTests.cpp. Commit e059f4a.

## Deviations
None. Test count is 364 (brief expected 354; base was 358 plus 6 new).

## Concerns
None.

## Fix round
- New SameValue helper in Schema.cpp: strings by ==, integers by their dump() text (what Literal writes), so -1 != 18446744073709551615 and 9223372036854775808 != -9223372036854775808. Used by Validate's `same` and the enum duplicate check. A uint64 above LLONG_MAX is a legal enum value (Literal writes its text), so {"enum":[-1,18446744073709551615]} is no longer refused; tested.
- New test case covers the accept bugs, the duplicate case, and huge integers against an Integer node (errors, no throw). The RED run of this test before the fix was not captured separately.
- Never-throws test now checks one error's path and message. Removed unused <optional> from SchemaTests.cpp.
- Not done: a `throw ContractError` after CheckValue's switch. Its cases leave via `break`, so a trailing throw fires on every call (verified: 7 tests failed); void function, no warning without it.
- `shown` kept: it renders strings raw in single quotes and uses the replace error handler for invalid UTF-8, unlike core::ShortJson (ASCII-escaped JSON).
- Result: [schema] all pass (463 assertions, 33 cases); ctest 100% of 365. Byte check: 0 non-ASCII, 0 control bytes in both files.
