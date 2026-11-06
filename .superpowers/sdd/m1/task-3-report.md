# Task 3 report: state schema

Status: DONE

## What was implemented

`morpheus::core::StateSchema` in the `core` static library, with its supporting types:
- `FieldType` enum (Int, Enum, String, List, Counter) and `FieldTypeName()`.
- `FieldDef` and `EntityTypeDef` structs.
- `StateSchema::FromJson()` parses `state_schema.json` and throws `DataError` naming the entity type or field at fault.
- `IsValidEntityId()` accepts non-empty ids using only `[a-z0-9_:]`.
- `CheckFieldValue()` returns "" for a valid value, otherwise a human-readable reason.
- `FindEntityType()` returns nullptr for an unknown type.
- `DefaultFields()` builds each field's default. It throws `ContractError` for an unknown type.
- `ValidateState()` throws `DataError` unless every entity has a valid id, a known type, and exactly its type's fields, all valid.

The code and tests were transcribed from the brief without changes.

## TDD evidence

RED: `cmake --build --preset tests` after writing `tests/core/StateSchemaTests.cpp` only.

```
C:\Users\user\Desktop\MorpheusEngine\tests\core\StateSchemaTests.cpp(7,1): error C1083: Cannot open include file: 'core/StateSchema.h': No such file or directory [C:\Users\user\Desktop\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
```

This matches the brief's expected failure.

GREEN: after adding the header, the source, and the CMake entry, `cmake --build --preset tests` exited 0 with no warnings or errors. Then `./build/tests/bin/Debug/morpheus_tests.exe "[schema]"` gave:

```
Filters: [schema]
Randomness seeded to: 3736954088
===============================================================================
All tests passed (35 assertions in 6 test cases)
```

Full suite: `ctest --preset tests` gave `100% tests passed out of 14`, including the 6 StateSchema cases and the existing ContentHash and Csv tests.

## Files changed

- Created `src/core/StateSchema.h`
- Created `src/core/StateSchema.cpp`
- Created `tests/core/StateSchemaTests.cpp`
- Modified `src/core/CMakeLists.txt` (added `StateSchema.cpp` after `Csv.cpp`)

Commit `d0a7290` "Add state schema with field validation and defaults" stages only `src/core` and `tests/core/StateSchemaTests.cpp`, as the brief specifies. The untracked `dotnet/` folder was not touched.

## Deviations from the brief

- Commit trailer: the brief's commit command uses `Co-Authored-By: Claude Opus 5.5`. The attribution reminder in the session says to use `Claude Haiku 5.5` from here on, so the commit uses `Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>`. Nothing else in the commit message changed.
- No code fixes were needed. The brief's code compiled under /W4 /WX on the first attempt.

## Self-review findings

- The compile is warning-free under /W4 /WX, and the `<format>` header works under the project's C++20 setting.
- The test expectations were checked against the implementation by hand before the build. Examples: `ValidateState` visits entities in sorted map order, so `"Bad.Id"` fails on its id before `"player"` is looked at. The `missingField` check reports `inventory`, the `extraField` check reports `wings`, and the `badType` check reports `dragon`. All of these matched when run.
- The `CHECK(... == std::vector<std::string>{"calm", "tense"})` line contains a comma outside parentheses. Catch2 v3's `CHECK` is variadic, so it compiles and passes.
- The `DefaultFields` `MORPHEUS_REQUIRE` call fires as a `ContractError`, which matches the global error policy for unknown engine-side type names.

## Concerns

- Git printed "LF will be replaced by CRLF the next time Git touches it" for the four files staged in the commit. This is a line-ending setting in this checkout, not a build problem. The content is LF as written from the brief. I did not change any git config.
- Nothing else is outstanding for this task.

## Fix round 1

### What changed

- `src/core/StateSchema.cpp`
  - `FromJson`: int `min`/`max` are range-checked against `int` limits before `get<int>()`. Unsigned values are compared as `std::uint64_t`, so a uint64 above LLONG_MAX cannot wrap. Out-of-range values throw `DataError` ("`<fieldWhere>`: 'max' 3000000000 is outside the int range ...").
  - `CheckFieldValue` Int: an unsigned value above LLONG_MAX is reported as "{value.dump()} is outside {min}..{max}". The value is read into a local `number` once. The case is now braced.
  - `CheckFieldValue` Counter: keys must be non-empty and contain no '.', otherwise "counter item names must be non-empty and contain no '.', got '<key>'".
  - Added `<cstdint>`, `<limits>`, `<utility>` includes.
- `src/core/StateSchema.h`: doc comment above `class StateSchema` states the counter item-name rule. No signatures changed.
- `tests/core/StateSchemaTests.cpp`: 3 new `[schema]` test cases (int bounds in FromJson, Int values in CheckFieldValue, Counter shapes).
  - Beyond the brief, I added bounds cases that wrap to a valid int without the fix (4294967301 wraps to 5; 2^63+5 wraps to 5), and an Int {-10..10} field with uint64 max, which wraps to -1. With only the brief's cases, the 3000000000 and uint64-max checks passed by accident via "min greater than max" or an accidental out-of-range rejection.

### Failing output before the fix (after tests written, before src change)

```
CHECK_THROWS_WITH( ... {"max", 4294967301LL} ... ) because no exception was thrown where one was expected
CHECK_THROWS_WITH( ... {"max", uint64{9223372036854775813}} ... ) because no exception was thrown where one was expected
CHECK_FALSE( CheckFieldValue(signedField, uint64 max).empty() )  with expansion: !true
CHECK_FALSE( CheckFieldValue(inventory, {{"a.b", 1}}).empty() )  with expansion: !true
CHECK_FALSE( CheckFieldValue(inventory, {{"", 1}}).empty() )     with expansion: !true
test cases:  9 |  6 passed | 3 failed
assertions: 46 | 41 passed | 5 failed
```

### Passing output after the fix

```
All tests passed (46 assertions in 9 test cases)      # morpheus_tests.exe "[schema]"
100% tests passed out of 17                           # ctest --preset tests
```

### Commands run

- `cmake --build --preset tests`
- `./build/tests/bin/Debug/morpheus_tests.exe "[schema]"` (before and after)
- `ctest --preset tests`
- `git add src/core/StateSchema.cpp src/core/StateSchema.h tests/core/StateSchemaTests.cpp`
- `git commit -m "Reject out-of-range integers and dotted counter keys in state schema" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"`

The first build after the fix failed with C2360 (initialization of `number` skipped by case label); fixed by bracing the Int case.
