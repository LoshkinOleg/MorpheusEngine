# Task 2 report
Implemented: added includes (<format>, <vector>, grammar/GbnfMatcher.h), JsonList helper, and the six [grammar] characterization tests verbatim from the brief in tests/turn/ResolveGrammarTests.cpp.
TDD: no RED step (characterization). GREEN: `cmake --build --preset tests` clean, no warnings; `morpheus_tests.exe "[grammar]"` -> All tests passed (147 assertions in 18 test cases). `ctest --preset tests` -> 100% passed out of 324.
Files: tests/turn/ResolveGrammarTests.cpp (+210). Commit 5281dc2.
Deviations: none; the plan's code compiled and passed as written. No grammar or matcher changes.
Concerns: none.

## Fix round
Added seven [grammar] cases (enum change on an existing entity; integer edges incl. create pos-int; counter item bounds incl. create; held-name prefix/suffix, empty counter, nothing-changeable world; short-text 120/121 for intent, stakes, reason, string value, create reason, list item, lookup query with raw multibyte, \u00e9 escapes, uppercase hex, invalid escapes, notes 400/401; review (true,false) and (false,true) variants; create shape minors and on_failure branch bound). Fixed the mislabeled npc:marta.disposition comment (tests entity existence). Every loop case has INFO(input).
Commands: `cmake --build --preset tests` (no warnings); `morpheus_tests.exe "[grammar]"` -> All tests passed (316 assertions in 25 test cases); `ctest --preset tests` -> see commit run below.
Surprises: none; today's grammars matched every expectation on the first run. Note: "int field out of range" is the grammar's 9-digit range only (schema min/max is not enforced by the grammar).

## Fix round 2
Cause: the tool decoded the backslash-u escapes I typed into raw e-acute bytes (it also decoded backslash-x sequences in my scripts). The escapes are now built as `BACKSLASH + "u00e9"` and `BACKSLASH + "u00C9"` (lower and upper hex), used for 120/121 on intent, stakes, reason, string value, create reason, list item, lookup query; 400/401 on notes; and 200/201 (problem) and 300/301 (digest) in the review variants. Raw multibyte stays `"\xC3\xA9"`.
Commands: `cmake --build --preset tests` (no warnings); `[grammar]` -> All tests passed (338 assertions in 25 test cases); `ctest --preset tests` -> 100% passed.
`LC_ALL=C grep -n '[^ -~]' tests/turn/ResolveGrammarTests.cpp` output: (empty)
`grep -n 'u00e9\|u00C9\|u00aF' tests/turn/ResolveGrammarTests.cpp`:
392:  R"({"intent":"caf\u00e9 \"quoted\"\n",...   (original, ASCII backslash-u in a raw string)
763:  const std::string escape = BACKSLASH + "u00e9";
764:  const std::string upperEscape = BACKSLASH + "u00C9";
776:  CHECK(accepts(upperEscape + BACKSLASH + "u00aF" + BACKSLASH + "uABCD"));
836:  const std::string escape = BACKSLASH + "u00e9";
837:  const std::string upperEscape = BACKSLASH + "u00C9";
