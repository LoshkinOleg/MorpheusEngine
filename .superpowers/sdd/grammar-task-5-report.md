# Task 5 report: Morpheus's grammars on the builder

Status: DONE

Commit: 803349c Build the resolve, lookup and review grammars on the grammar builder

## What was implemented

- `tests/turn/ResolveGrammarTests.cpp`: the exact-text tests updated to the builder's text, exactly as the brief's
  Step 1 lists (parentheses around enums, `items-N`, lists that may be empty, and the review objects written as
  key/value fragments). The disposition line was replaced in all three tests (one `replace_all`, three hits).
- `src/turn/CMakeLists.txt`: `morpheus_turn` links `morpheus_grammar` (src/grammar is added before src/turn in the
  top-level CMakeLists.txt, so no ordering change was needed).
- `src/turn/ResolveGrammar.h`: replaced verbatim with the brief's header (static_assert removed,
  `constexpr long long MAX_INT_MAGNITUDE = 999999999;` added, comments updated).
- `src/turn/ResolveGrammar.cpp`: replaced verbatim with the brief's code. The anonymous namespace (`TERMINALS`,
  `Terminals`, the private `Literal`, `Alternatives`, `ListOf`) is gone; everything is built with `grammar::*` and
  `grammar::GrammarBuilder`.

The brief's code compiled as written under /W4 /WX: no compile fixes were needed.

## TDD evidence

Baseline (before any edit), `./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`:
`All tests passed (338 assertions in 25 test cases)`

RED (tests updated, old implementation), `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`:
```
test cases:  25 |  18 passed |  7 failed
```
The 7 failing cases are exactly the brief's list:
- The resolve grammar names the world's stats, difficulties and field shapes (lines 197, 206)
- The resolve grammar follows the live state and the memory mode (233; the `CountOccurrences` check too)
- The resolve grammar bounds every list (246-250)
- The resolve grammar covers list fields and enum sets on existing entities (265)
- The resolve grammar escapes enum values (281)
- Held counter items are matched by name next to the generic item rule (296)
- The review grammar asks for the verdict, the digest or both (359, 367)
All characterization cases passed.

GREEN (new implementation), same command: builds with no warnings;
`All tests passed (338 assertions in 25 test cases)`.
The characterization tests (Task 2) were not touched and pass before and after: the language is unchanged.

Step 7, `./build/tests/bin/Debug/morpheus_tests.exe "[replay],[session],[review],[model_seat]"`:
`All tests passed (4287 assertions in 69 test cases)`.

Step 8, `grep -n "Terminals\|TERMINALS\|namespace {" src/turn/ResolveGrammar.cpp`: no output (exit 1).

Step 9, `ctest --preset tests`: `100% tests passed out of 342`.

## Escape-sequence checks

For `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp` and `tests/turn/ResolveGrammarTests.cpp`:
- `LC_ALL=C grep -n $'[\x80-\xFF]' <file>`: no output for each.
- `LC_ALL=C grep -c $'[\x01-\x08\x0b\x0c\x0e-\x1f\x7f]' <file>`: `0` for each.
The new code writes no backslash-x or backslash-u escapes (the `char` rule now lives in src/grammar/Gbnf.cpp, not
touched here); the test edits only added parentheses and comments to raw strings.

## Files changed

- src/turn/CMakeLists.txt
- src/turn/ResolveGrammar.h
- src/turn/ResolveGrammar.cpp
- tests/turn/ResolveGrammarTests.cpp

## Deviations from the brief

- Test counts differ from the brief's expectations, not the behaviour: `[grammar]` has 25 test cases (the brief said
  18: twelve existing plus six characterization; Task 2 ended with 13 characterization cases), the RED run passed 18
  (not "the six characterization tests" alone, but every case without exact text) and the full suite has 342 tests
  (the brief said 335; the dispatch said 342 at the base commit, which matches).
- Commit trailer names Opus (`Co-Authored-By: Claude Opus 5.5`), as the global constraints say for an Opus
  implementer.

## Concerns

None. Notes for review:
- The resolve grammar now always defines `short-text`, `int`, `nonzero-int`, `pos-int`, `item`, `new-id`,
  `text-list`, `counter-value` and `reason` (as the old shared terminals did), and `long-text` only in assisted mode;
  the lookup and review grammars no longer carry the unused resolve terminals. Unused rules do not change the
  language.
- Rule order changed (enum-N, items-N and change-N come first), as the plan's decisions state.
