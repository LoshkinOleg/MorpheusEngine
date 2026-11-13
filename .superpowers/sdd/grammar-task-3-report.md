# Task 3 report: morpheus_grammar library and builder

Commit: 55fd919 "Add morpheus_grammar with the GBNF builder"

## Implemented
src/grammar/ (CMakeLists.txt, Gbnf.h, Gbnf.cpp) verbatim from the brief: Literal, JsonString, Enum, ListOf, Object, Alternatives, ObjectField, GrammarBuilder::Rule/Text. CMake: `add_subdirectory(src/grammar)` after context; `morpheus_grammar` added to morpheus_tests link line. Tests: tests/grammar/GbnfTests.cpp (8 cases, verbatim).

## TDD
RED: `cmake --build --preset tests` ->
`D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(7,1): error C1083: Cannot open include file: 'grammar/Gbnf.h': No such file or directory`
(The first attempt, with the CMake wiring but no src/grammar directory, failed at configure; I then created src/grammar/CMakeLists.txt and a one-line placeholder Gbnf.cpp so the build reached the expected C1083.)

GREEN: `cmake --build --preset tests` (no warnings, no errors), then `./build/tests/bin/Debug/morpheus_tests.exe "[gbnf]"`:
`All tests passed (84 assertions in 8 test cases)`
Full: `ctest --preset tests`: `100% tests passed out of 339`.
Consumer-check: `cmake -S cmake/consumer-check -B <scratchpad>/cc` configured and generated fine (it does not list the grammar target; unchanged).

## Escape check
- Tool decoded one escape: the Write tool turned `\u00e9` in `text.Accepts(R"("\u00e9\n\"")")` (GbnfTests.cpp line 49) into a real e-acute. Caught by the non-ASCII grep, repaired with a python byte replacement using chr(92).
- Final checks on all three files (Gbnf.cpp, Gbnf.h, GbnfTests.cpp): `LC_ALL=C grep -n $'[\x80-\xFF]'` prints nothing; a python byte scan reports 0 control bytes (other than tab/CR/LF) and 0 bytes >= 127 (the brief's `grep -c $'[--]'` form errored in this shell, so the byte scan replaced it). `grep -n 'x7F'`: Gbnf.cpp line 20 still `[^"\\\x7F\x00-\x1F]`; test lines 16, 31, 50, 54 and 49 (`\u00e9`) show the literal backslash sequences.

## Deviations
- Test count: 339 passed (331 base + 8), not the brief's 332; the brief's number was a plan estimate.
- RED ordering as noted above (placeholder Gbnf.cpp).
- No compile fixes were needed; the plan code compiled warning-free.

## Concerns
None. The consumer-check comment ("five engine libraries") is deliberately left for the documentation task per the constraints.
