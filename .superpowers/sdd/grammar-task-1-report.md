# Task 1 report: test-only GBNF matcher

Implemented tests/grammar/GbnfMatcher.h and tests/grammar/GbnfMatcherTests.cpp verbatim from the brief.

## TDD
RED: `cmake --build --preset tests` (header withheld)
`GbnfMatcherTests.cpp(5,1): error C1083: Cannot open include file: 'grammar/GbnfMatcher.h': No such file or directory`

GREEN: `cmake --build --preset tests` (no warnings), then `./build/tests/bin/Debug/morpheus_tests.exe "[gbnf-matcher]"`
`All tests passed (35 assertions in 3 test cases)`
`ctest --preset tests`: `100% tests passed out of 318`

## Files
tests/grammar/GbnfMatcher.h, tests/grammar/GbnfMatcherTests.cpp. Commit 9b3899d.

## Deviations
None: the plan's code compiled under /W4 /WX unchanged.

## Concerns
None. (git warns LF will become CRLF; harmless.)
