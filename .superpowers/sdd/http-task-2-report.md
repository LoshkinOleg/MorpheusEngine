# Task 2 report: grammar::Check

Status: DONE

Commit: 6fc777b Promote the GBNF matcher into morpheus_grammar as grammar::Check

## What was implemented

- `src/grammar/Check.h` and `src/grammar/Check.cpp`: `std::string grammar::Check(const std::string& grammar, std::string_view text)`, exactly as in the brief. It is memoised by (node, position), reads repetition counts strictly, and throws `core::ContractError` for grammars it cannot read. It returns `""`, "the grammar rejects '<c>' at the start", "the grammar rejects '<c>' after '<text>'" (the last 60 bytes, cut on a whole character, with "..." in front when cut) or "the text ends before the grammar does". `<c>` is the code point at the furthest position any partial match reached.
- `src/grammar/CMakeLists.txt`: `Check.cpp` added to `morpheus_grammar`.
- `tests/grammar/CheckTests.cpp`: the 8 test cases from the brief, verbatim.
- The switch from the matcher to `grammar::Check`, by the brief's mechanical rule, in:
  - `tests/grammar/GbnfTests.cpp`: the helper becomes `GrammarFor` and every `MatcherFor` matcher is converted.
  - `tests/grammar/SchemaTests.cpp`: the two `matcher`s.
  - `tests/grammar/PropertyTests.cpp`: `CheckRandomValues` is now the brief's version.
  - `tests/turn/ResolveGrammarTests.cpp`: every matcher, and the characterization comment now says "checked with grammar::Check".
- Deleted `tests/grammar/GbnfMatcher.h` and `tests/grammar/GbnfMatcherTests.cpp` with `git rm`.
- `tests/llama/LlamaGrammarTests.cpp`:
  - the include is switched;
  - the `VerdictOf` helper is added;
  - the random-values test case is replaced with "llama.cpp and grammar::Check accept the random values of each test schema";
  - the invalid-actions loop now compares the verdict kinds;
  - the IntRange test case is replaced with "llama.cpp and grammar::Check agree on IntRange grammars".
  All of this text was taken verbatim from the brief.

I applied the switch with a small Python script. It reads C++ string literals, raw strings included, so it matches parentheses correctly. The rules it applied:
- `const test::GbnfMatcher N(E);` becomes `const std::string N = E;`.
- `const test::GbnfMatcher N = MatcherFor(E);` becomes `const std::string N = GrammarFor(E);`.
- `N.Accepts(T)` becomes `grammar::Check(N, T).empty()`.

The code blocks for Check.h, Check.cpp, CheckTests.cpp and the llama tests were copied byte for byte from the brief file (sed and Python line ranges), so no escape could be decoded on the way.

## TDD evidence

RED (CheckTests.cpp written, no Check.h yet):
```
$ cmake --build --preset tests
D:\MorpheusEngine\tests\grammar\CheckTests.cpp(7,1): error C1083: Cannot open include file: 'grammar/Check.h': No such file or directory [D:\MorpheusEngine\build\tests\tests\morpheus_tests.vcxproj]
```

GREEN (new tests):
```
$ cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar-check]"
Filters: [grammar-check]
All tests passed (73 assertions in 8 test cases)
```

Full fast suite after the switch (built with no warnings):
```
$ ctest --preset tests
100% tests passed out of 375
Total Test time (real) =  36.37 sec
```
That is 370, minus the matcher's 3, plus 8. The brute-force IntRange test takes 7.61 s in Debug. "grammar::Check stays fast on nested repetitions over a long text" takes 0.55 s, under its 5 s limit.

Dev build: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe` built all four targets with no compiler warnings.

Model-backed grammar tests, run from the repo root with `MORPHEUS_TEST_CHAT_MODEL` and `MORPHEUS_TEST_EMBED_MODEL` set to the two models:
```
$ ./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*,LlamaCppBackend::CheckGrammar*"
All tests passed (4928 assertions in 8 test cases)
```
None were skipped. llama.cpp and `grammar::Check` agree on every probe: the 1000 random values per schema, the invalid catalog actions (verdict kind) and every IntRange probe (verdict kind and in-range). The "failed to parse grammar" lines in the log come from the existing tests that check llama.cpp refuses grammars over its repetition limits and an unclosed group.

## Byte checks

`check_bytes` on all 9 committed source and test files printed nothing.

`LC_ALL=C grep -n $'[\x80-\xFF]' <file>` printed nothing for every file.

The control-byte count (`LC_ALL=C grep -c`, control bytes other than tab and newline) is 0 for every file:
```
src/grammar/Check.h 0
src/grammar/Check.cpp 0
src/grammar/CMakeLists.txt 0
tests/grammar/CheckTests.cpp 0
tests/grammar/GbnfTests.cpp 0
tests/grammar/SchemaTests.cpp 0
tests/grammar/PropertyTests.cpp 0
tests/turn/ResolveGrammarTests.cpp 0
tests/llama/LlamaGrammarTests.cpp 0
```

The escapes are still literal, for example in CheckTests.cpp:
```
39: char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
43: CHECK(grammar::Check(jsonString, "\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"").empty());
44: CHECK(grammar::Check(jsonString, R"("\n\"\u00e9")").empty());
```

`grep -rn "GbnfMatcher\|\.Accepts(" tests src` prints nothing.

## Deviations (mechanical, within the brief's intent)

1. **More matchers than the brief's list.** ResolveGrammarTests.cpp has more matchers than the brief names (`held`, `empty`, `none`, `lists`, `branches`, and second `engine`, `assisted`, `lookup`, `check` and `digest` instances). The same rule converted all of them.
2. **Include order.** In SchemaTests.cpp the in-place include swap left `grammar/Check.h` after `grammar/Gbnf.h`, so I reordered it, as the brief asks. GbnfTests.cpp got the same fix. In ResolveGrammarTests.cpp the include sits where the old one was: that block was already unsorted (`TestPaths.h`, then `grammar/...`, then `core/...`), and I left its order alone.
3. **Continuation lines.** Where a converted statement spans lines, the continuation lines were realigned to the new opening:
   - GbnfTests.cpp: the `either` declaration;
   - ResolveGrammarTests.cpp: lines 455, 522 and 526, and the `held` and `empty` declarations.
   A trailing space after `grammar::Check(none,` was removed.
4. **Test filter in Step 7.** The brief's filter, `"llama.cpp*" "LlamaCppBackend::CheckGrammar*"` as two arguments, matched no tests: Catch2 v3 joins them with a space into one AND filter. I used the comma OR form, `"llama.cpp*,LlamaCppBackend::CheckGrammar*"`, which matched the 8 intended cases.
5. **Commit staging.** The brief's `git add` of the two deleted paths would fail, because they were already removed with `git rm`. I staged the three new files with `git add`, then committed with `git commit <all 11 paths>`. Only the task's files are in the commit.

## Concerns

- **Docs still describe the matcher.** `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md:183` still names the test-only matcher `tests/grammar/GbnfMatcher.h`. The plan updates the docs in a later task (plan line ~5990, the grep at ~6220), so I did not touch them here.
- **Brute-force test timing.** `grammar::Check` parses the grammar again on every call, where the old matcher parsed it once per matcher object. The IntRange brute-force test still runs in 7.6 s in Debug; I had no baseline timing from before to compare against.
