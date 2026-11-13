# Task 8 report
Implemented tests/grammar/RandomValue.h and tests/grammar/PropertyTests.cpp verbatim from the brief (written via quoted heredoc).
RED: `cmake --build --preset tests` -> error C1083: Cannot open include file: 'grammar/RandomValue.h'.
GREEN: `morpheus_tests.exe "[property]"` -> All tests passed (4013 assertions in 3 test cases). Build had no warnings.
Full suite: ctest --preset tests -> 100% passed out of 368 (365 + 3).
Escape checks: non-ASCII grep printed nothing; control-byte count 0 for both files; `\x01`, `\x1F`, `\xF0...` remain literal in RandomValue.h.
Commit 17a4630. No deviations. No generator/Validate/grammar disagreements found. Git only warns about LF->CRLF.

## Fix round
1. PropertyTests.cpp: the escaped check now uses `const std::string specials = {'"', '\', '\n', '\t'};` (quote, backslash, newline, tab); find_first_of(specials). Verified by sed and grep of the line.
2. RandomValue.h: no std distributions left. RandomBelow(random, count) is unbiased rejection sampling on mt19937_64 raw output; RandomBetween (1/4 each end, else low + RandomBelow(span), span unsigned, span 0 = full range uses raw draw), pick, string character choice and printable ASCII use it.
Results: [property] passes (4013 assertions, 3 cases), coverage assertions unchanged and still pass; ctest 368/368. Byte checks: no non-ASCII, 0 control bytes in both files.
