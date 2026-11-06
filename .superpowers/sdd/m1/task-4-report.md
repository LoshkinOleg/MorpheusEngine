# Task 4 report: State changes (set / add / remove / create)

## Implemented
`src/core/Changes.h`, `src/core/Changes.cpp` and `tests/core/ChangesTests.cpp`, transcribed verbatim from the brief. `Changes.cpp` is added to `src/core/CMakeLists.txt` after `StateSchema.cpp`. This provides `ChangeOp`, `ChangeOpName`, `Change`, `AppliedChange`, `RejectedChange`, `ApplyResult`, `ParseChange`, `ChangeToJson` and `ApplyChanges`.

## TDD evidence
RED: `cmake --build --preset tests`
```
ChangesTests.cpp
...\tests\core\ChangesTests.cpp(6,1): error C1083: Cannot open include file: 'core/Changes.h': No such file or directory
```
GREEN: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[changes]"`
```
Filters: [changes]
All tests passed (49 assertions in 10 test cases)
```
Full suite: `ctest --preset tests` gave `100% tests passed out of 27`.

## Files changed
- src/core/Changes.h (new)
- src/core/Changes.cpp (new)
- src/core/CMakeLists.txt (modified)
- tests/core/ChangesTests.cpp (new)

Commit: 17b4be0 "Add validated state changes (set, add, remove, create)". The `dotnet/` folder was not staged.

## Deviations from the brief
None. The code compiled cleanly under /W4 /WX with no changes. The commit trailer uses "Claude Sonnet 5.5", as the attribution reminder required, instead of the brief's "Claude Opus 5.5".

## Self-review findings
- The brief's code was copied exactly, and every brief test passes.
- The code does not throw on bad model input. The only throws are the `ContractError` for a non-array batch and for a state entity with an unknown type, and both are engine bugs by design.
- Every check runs before the state is modified, so a rejected change leaves no partial edit.

## Concerns (not fixed, because the brief specifies this code)
- Overflow in `add` is not guarded. An LLM-proposed huge integer, for example 9e18 or an unsigned value above LLONG_MAX, can overflow `long long` in `field.get<long long>() + change.value.get<long long>()`. This applies to the int-field path and the counter-item path. That is undefined behavior, or a wrapped result that then gets clamped. The same range hardening that was applied to `CheckFieldValue` in Task 3 could be applied here, for example by rejecting values outside the int range or using checked arithmetic. A counter item that grows past the range `CheckFieldValue` allows is possible in the same way.
- `std::clamp<long long>(wanted, def.min, def.max)` assumes min <= max. Schema parsing is expected to enforce that.

## Fix round 1

### What changed
- `src/core/Changes.cpp`: new `IsIntRangeInteger` helper (checks `is_number_unsigned()` first, then signed range [INT_MIN, INT_MAX]). `add` on an int field and on a counter item use it, so uint64 values above LLONG_MAX never reach `get<long long>()` and sums cannot overflow `long long`. Counter `add` also rejects a result above INT_MAX (`count of '<item>' would exceed 2147483647`). Added `<limits>` and `<cstdint>` includes.
- `src/core/StateSchema.cpp`: `CheckFieldValue` Counter branch now requires an integer in `1..INT_MAX`, checking unsigned before `get<long long>()`.
- `src/core/StateSchema.h`, `src/core/Changes.h`: doc comments updated.
- Tests: `ChangesTests.cpp` (LLONG_MAX, LLONG_MIN, uint64 max, 2.5 on health; rope +INT_MAX; rope +uint64 max; INT_MAX add still clamps to 10; remove with object value on list rejected), `StateSchemaTests.cpp` (counter 2147483648 rejected, 2147483647 accepted).

### Failing output before the fix
```
CheckFieldValue caps counter counts at INT_MAX
  StateSchemaTests.cpp(192): FAILED: CHECK_FALSE( ...{"rope", 2147483648LL}... .empty() ) with expansion: !true
add rejects integers outside the int range instead of wrapping or overflowing
  ChangesTests.cpp(147): FAILED: REQUIRE( result.rejected.size() == 1 ) with expansion: 0 == 1
  with message: value.dump() := "9223372036854775807"
add on a counter item rejects out-of-range values and counts above INT_MAX
  ChangesTests.cpp(167): FAILED: REQUIRE( overflow.rejected.size() == 1 ) with expansion: 0 == 1
test cases:  23 | 20 passed | 3 failed
assertions: 102 | 99 passed | 3 failed
```

### Passing output after the fix
```
All tests passed (128 assertions in 23 test cases)     ([changes],[schema])
100% tests passed out of 31                            (ctest --preset tests)
```

### Commands
- `cmake --build --preset tests`
- `./build/tests/bin/Debug/morpheus_tests.exe "[changes],[schema]"`
- `ctest --preset tests`
- `git add <6 files>; git commit`
