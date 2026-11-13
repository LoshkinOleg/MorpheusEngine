# Task 4 report: IntRange

Status: DONE

Commit: 7219067 Add IntRange: exact decimal integer ranges as GBNF

## What was implemented

- `grammar::IntRange(long long min, long long max)` in `src/grammar/Gbnf.h/.cpp`, exactly as the brief gives it:
  the range is split at zero (negative side as `"-"` plus a magnitude, `"0"`, positive side), each side's magnitudes
  (unsigned long long, so |LLONG_MIN| = 2^63 fits via `0ULL - static_cast<unsigned long long>(v)`) are split into
  digit-count bands (`MagnitudeAlternatives`), and each partial band is written digit by digit
  (`SameLengthAlternatives`, recursive). Helpers `AnyDigits`, `SameLengthAlternatives`, `MagnitudeAlternatives` in the
  anonymous namespace. `ContractError` when `min > max`.
- The brief's three test cases appended to `tests/grammar/GbnfTests.cpp` (tag `[int-range]`), with the brief's include
  changes.

The brief's code and tests were applied verbatim (extracted programmatically from the brief's code blocks, so no tool
could alter them).

## Hand-computed texts vs the algorithm

Before building I traced every hand-computed expected text through the algorithm by hand (int, pos-int, negative
half, single values, -1..1, 0..9, 10..99, 0..255, -15..123, 37..120): all agree. The build then confirmed it: every
exact-text CHECK passed on the first run, and the brute force (matcher) passed too. No expected text or code needed a
fix.

## TDD evidence

RED (tests written, implementation absent):

```
$ cmake --build --preset tests 2>&1 | grep -E "error|warning" | head -10
D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(176,5): error C2039: 'IntRange': is not a member of 'morpheus::grammar' [D:\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(176,5): error C3861: 'IntRange': identifier not found [D:\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(176,5): warning C4130: '==': logical operation on address of string constant [D:\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(177,5): error C2039: 'IntRange': is not a member of 'morpheus::grammar' [D:\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(177,5): error C3861: 'IntRange': identifier not found [D:\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
...
```

(The C4130 warnings are a consequence of the missing identifier and disappear with it.)

GREEN:

```
$ cmake --build --preset tests 2>&1 | grep -E "error|warning C"
(nothing)
$ ./build/tests/bin/Debug/morpheus_tests.exe "[int-range]"
Filters: [int-range]
Randomness seeded to: 2195917365
===============================================================================
All tests passed (43089 assertions in 3 test cases)
```

Full suite:

```
$ cmake --build --preset tests 2>&1 | grep -ci "warning"
0
$ ctest --preset tests
100% tests passed out of 342
Total Test time (real) =  21.78 sec
```

(342 = the 339 at 55fd919 plus the 3 new test cases; the brief's "335" predates earlier tasks.)

## Escape-sequence checks (after every write)

```
== src/grammar/Gbnf.h          high-byte grep: (nothing)   control-byte count: 0
== src/grammar/Gbnf.cpp        high-byte grep: (nothing)   control-byte count: 0
== tests/grammar/GbnfTests.cpp high-byte grep: (nothing)   control-byte count: 0
$ grep -n 'x7F' src/grammar/Gbnf.cpp
21:    R"gbnf(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))gbnf";
```

The `char` rule's escapes are intact literal backslash sequences. The new code and tests contain no backslash escapes.

## Files changed

- src/grammar/Gbnf.h
- src/grammar/Gbnf.cpp
- tests/grammar/GbnfTests.cpp

## Deviations

- None in code. The commit's Co-Authored-By line names Claude Opus 5.5 (the implementing model), as the global
  constraints direct, instead of the brief's Sonnet line.
- Expected suite count is 342, not the brief's 335 (stale count).

## Concerns

None.
