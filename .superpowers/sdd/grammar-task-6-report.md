# Task 6 report: FromSchema

Status: DONE

Commit: e69ad40 Add FromSchema: GBNF for a subset of JSON Schema (base 803349c)

## What I implemented

- `src/grammar/Schema.h`: `grammar::FromSchema(const nlohmann::json&)` with the brief's subset comment, plus one sentence on the repetition limits.
- `src/grammar/Schema.cpp`: the brief's code, verbatim except for the limit checks below: `IGNORED_KEYWORDS`/`SUBSET_KEYWORDS`/`KIND_KEYWORDS`, `struct Node`, `PointerToken`, `Compile` (throws `DataError` "schema at <pointer>: <problem>") and `Expression`, all in an anonymous namespace for Task 7.
- `src/grammar/CMakeLists.txt`: `Schema.cpp` added to `morpheus_grammar`.
- `tests/grammar/TestSchemas.h`: `test::CatalogSchema()` and `test::EveryKeywordSchema()`, verbatim from the brief.
- `tests/grammar/SchemaTests.cpp`: the brief's 13 test cases, plus the limit cases.

### Controller-requested addition: llama.cpp's repetition limit

- `src/grammar/Gbnf.h`: `constexpr int GBNF_MAX_REPETITION = 2000;` near the top of the namespace, with a comment naming llama.cpp's `MAX_REPETITION_THRESHOLD` in `llama-grammar.cpp`.
- `src/grammar/Gbnf.cpp`:
  - `JsonString` requires `maxChars <= GBNF_MAX_REPETITION`.
  - `ListOf` requires `maxItems - 1 <= GBNF_MAX_REPETITION`, the repetition count it actually writes.
  - Both use `MORPHEUS_REQUIRE`, so a breach is a ContractError.
- `Schema.cpp` (`Compile`): the check runs before the builder is ever called, and Task 7's `Validate` will share it through `Compile`. The local lambda `beyondRepetitionLimit` builds the DataError; it has two callers.
  - String: `maxLength > 2000` gives `schema at /maxLength: 'maxLength' 2001 is above 2000, the most llama.cpp's grammar repetition limit (2000) allows`. This check sits right after the "needs 'maxLength'" check.
  - Array: `maxItems - 1 > 2000` gives `schema at /maxItems: 'maxItems' 2002 is above 2001, the most llama.cpp's grammar repetition limit (2000) allows`. This check sits right after the "at least 1" check.
- Tests:
  - `tests/grammar/GbnfTests.cpp` gets 2 new `[gbnf]` test cases, "JsonString stays within llama.cpp's repetition limit" and "ListOf stays within llama.cpp's repetition limit". Each checks the exact text at the limit (`char{0,2000}`, `("," x){0,2000}`) and a ContractError one past it.
  - In `SchemaTests.cpp`, the strings and arrays rule tests each gain a CHECK_NOTHROW at the limit (maxLength 2000, maxItems 2001) and a DataError check one past it.

## TDD evidence

RED 1 (all tests written, no implementation): `cmake --build --preset tests 2>&1 | grep -E "error|warning|Error"`
```
D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(66,5): error C2039: 'GBNF_MAX_REPETITION': is not a member of 'morpheus::grammar'
D:\MorpheusEngine\tests\grammar\GbnfTests.cpp(66,5): error C2065: 'GBNF_MAX_REPETITION': undeclared identifier
... (same for lines 67, 112, 113)
D:\MorpheusEngine\tests\grammar\SchemaTests.cpp(12,1): error C1083: Cannot open include file: 'grammar/Schema.h': No such file or directory
```

RED 2 (runtime RED for the builder limits). I added only the constant to Gbnf.h and moved SchemaTests.cpp aside for one build (it was restored straight after). Command: `./build/tests/bin/Debug/morpheus_tests.exe "[gbnf]"`
```
ListOf stays within llama.cpp's repetition limit
GbnfTests.cpp(113): FAILED:
  CHECK_THROWS_AS( grammar::ListOf("x", 1, grammar::GBNF_MAX_REPETITION + 2), core::ContractError )
because no exception was thrown where one was expected:
JsonString stays within llama.cpp's repetition limit
GbnfTests.cpp(67): FAILED:
  CHECK_THROWS_AS( grammar::JsonString(0, grammar::GBNF_MAX_REPETITION + 1), core::ContractError )
because no exception was thrown where one was expected:
test cases: 10 |  8 passed | 2 failed
assertions: 88 | 86 passed | 2 failed
```

GREEN: `cmake --build --preset tests` built with no warnings or errors.
- `./build/tests/bin/Debug/morpheus_tests.exe "FromSchema*,The catalog*,The grammar of every*"` gave `All tests passed (197 assertions in 13 test cases)`.
- `./build/tests/bin/Debug/morpheus_tests.exe "[gbnf]"` gave `All tests passed (88 assertions in 10 test cases)`.
- `./build/tests/bin/Debug/morpheus_tests.exe "[schema]"` gave `All tests passed (259 assertions in 26 test cases)`. The 13 existing StateSchema tests are also tagged `[schema]`, so the brief's "13 test cases" count holds only for the new tests.
- `ctest --preset tests` gave `100% tests passed out of 357` (342 + 13 schema + 2 gbnf; the brief expected 348 before the addition).

## Escape-sequence checks

My Write tool decoded `\u00e9` in SchemaTests.cpp line 163 (now 166) into a raw "é". I fixed it with sed (`\x5c` for the backslash) and confirmed the bytes with od: `" n a m e " : " \ u 0 0 e 9 \ " "`. A heredoc write had failed earlier with a shell quoting error, so I used the Write tool and fixed the result.

Final checks on every changed file (SchemaTests.cpp, TestSchemas.h, GbnfTests.cpp, Gbnf.h, Gbnf.cpp, Schema.h, Schema.cpp, CMakeLists.txt):
- `LC_ALL=C grep -n $'[\x80-\xFF]'` printed nothing for every file.
- The control-byte count `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'` printed 0 for every file.

Intended escapes, still literal:
```
SchemaTests.cpp:53:        item += "\xC3\xA9";
SchemaTests.cpp:90:char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
SchemaTests.cpp:166:        with(R"("name":"ab")", R"("name":"\u00e9\"")"),
SchemaTests.cpp:167:        with(R"("name":"ab")", "\"name\":\"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\""),
GbnfTests.cpp: lines 20, 35, 53, 54, 58 unchanged (\x7F, \xC3\xA9, \u00e9 still literal)
Gbnf.cpp CHAR_RULE: od shows `[ ^ " \ \ \ x 7 F \ x 0 0 - \ x 1 F ]` (literal)
```

## Deviations from the brief

1. `SchemaError` test helper:
   - The brief's `FAIL(...); return {};` failed the build with C4702 (unreachable code) under /WX at `SchemaTests.cpp(36)`.
   - It now stores the message in a `std::optional<std::string>`, then `INFO("FromSchema accepted the schema " << text); REQUIRE(message.has_value()); return *message;`.
   - The behaviour is the same: the test fails and names the schema when nothing is thrown. `<optional>` is now included.
2. The repetition-limit addition described above, which the controller requested.
3. Gbnf.cpp was edited through a Python script that wrote CRLF line endings; I converted the file back to LF. git autocrlf is true, so the committed diff is only the 3 added lines.

The brief's Schema.cpp compiled under /W4 /WX with no other changes.

## Files changed

- D:/MorpheusEngine/src/grammar/Gbnf.h
- D:/MorpheusEngine/src/grammar/Gbnf.cpp
- D:/MorpheusEngine/src/grammar/Schema.h (new)
- D:/MorpheusEngine/src/grammar/Schema.cpp (new)
- D:/MorpheusEngine/src/grammar/CMakeLists.txt
- D:/MorpheusEngine/tests/grammar/TestSchemas.h (new)
- D:/MorpheusEngine/tests/grammar/SchemaTests.cpp (new)
- D:/MorpheusEngine/tests/grammar/GbnfTests.cpp

## Concerns

- The `[schema]` tag is shared with the existing StateSchema tests (core), so `"[schema]"` runs 26 test cases, not 13. Renaming the tag (for example `[from-schema]`) is the controller's call; I kept the brief's tag.
- I wrote the limit DataError wording myself because the controller gave none. Task 7's `Validate` will throw the same messages through `Compile`.
- I did not run the `asan` preset.

---

# Fix round (llama.cpp's real repetition limits)

Status: DONE. Commit: 8ee1976 Keep FromSchema grammars within what llama.cpp loads (on top of e69ad40).

## What changed

- **Constants.** In `src/grammar/Gbnf.h`, `GBNF_MAX_REPETITION` is replaced by `GBNF_MAX_STRING_CHARS = 1999` and `GBNF_MAX_LIST_ITEMS = 1000`.
  - The grouped comment cites both of llama-grammar.cpp's checks against `MAX_REPETITION_THRESHOLD` (2000): a count `{m,n}` above 2000, and `n_prev_rules * max >= 2000` in `handle_repetitions`.
  - It also explains what counts as a symbol and derives each bound: `maxChars * 1 < 2000`, and `(maxItems - 1) * 2 < 2000`.
  - The doc comments of `JsonString` and `ListOf` name their limit, and `ListOf`'s also names its element rule.
- **`JsonString`** requires `maxChars <= GBNF_MAX_STRING_CHARS` (ContractError).
- **`ListOf`:**
  - It requires `maxItems <= GBNF_MAX_LIST_ITEMS`.
  - Its element must be a rule reference (`[a-z0-9-]+`) or one string literal. The literal check is an inline lambda: an opening quote, then characters or backslash escapes, then the closing quote as the last byte.
  - The rule-name check now has two callers (`ListOf` and `GrammarBuilder::Rule`), so it moved into the anonymous-namespace helper `IsRuleName`.
- **Morpheus's grammars needed no change.** Every `ListOf` call in `src/turn/ResolveGrammar.cpp` already passes a rule name (`check`, `change`, `long-text`, `short-text`, `lookup`, `problem-text`). Every constant is far inside the limits: lists at most 8 items, strings at most 400 characters.
- **`FromSchema`:**
  - `Expression(node, builder, itemRules)` makes each array's item expression a rule `items-N`. N counts in the order the rules are added, so inner arrays come first. `ListOf` therefore always repeats a single reference.
  - `FromSchema` now owns the `GrammarBuilder`.
  - No other repeated expression needed a rule. `JsonString` repeats the reference `char`, `IntRange` repeats inline `[0-9]` classes at most 18 times, and the `char` rule's `{4}` is fixed.
  - `Compile`'s DataErrors are now `'maxLength' N is above 1999, the most a grammar llama.cpp loads can hold` and `'maxItems' N is above 1000, the most a grammar llama.cpp loads can hold`, at `/maxLength` and `/maxItems`.
- **`Schema.h`** describes both limits and the `items-N` rules.

## Tests

- **`GbnfTests.cpp` boundaries:**
  - `JsonString(0, 1999)` gives `char{0,1999}`, and 2000 throws.
  - `ListOf("x", 1, 1000)` gives `("," x){0,999}`, and 1001 throws.
- **New test case** "ListOf takes a rule reference or one string literal as its element":
  - Accepted: `item-2`, and a literal holding an escaped quote.
  - Refused: an empty element, `Item`, `x y`, a `JsonString`, an `Enum`, two literals, an unterminated literal, and a literal ending in an escaped quote.
- **`SchemaTests.cpp`:**
  - The array expectations use `ArrayGrammarOf`, a new helper with two callers that names the item `items-0`.
  - An exact-text check pins nested array numbering (inner `items-0`, outer `items-1`).
  - Boundaries: maxLength 1999 is accepted and 2000 refused; maxItems 1000 is accepted and 1001 refused.
  - The review minors: matcher rejections of `"version":"2"` and `"codes":["3"]`, plus "does not apply to" DataErrors for a oneOf (`maxItems`), a string (`items`) and an array (`maxLength`).
- **New `[llama]` test** "llama.cpp loads FromSchema grammars at the repetition limits", in `tests/llama/LlamaGrammarTests.cpp`. It uses `CheckGrammar`, which must return "" for each case:
  - a full string of 1999 characters, maxLength 1999;
  - a full list of 1000 strings, each maxLength 1999;
  - a list of up to 1000 objects, each holding that list;
  - the catalog schema;
  - a list of 50 strings of up to 48 characters.

  It also pins what llama.cpp refuses (ContractError from `CheckGrammar`), with grammars written by hand:
  - `char{0,2000}` between quotes;
  - the old inline list `"[" <string> ("," <string>){0,49} "]"`, with `<string>` being `JsonString(0, 48)`.

  In the same file, the existing CheckGrammar test's local variable `grammar` is renamed `yesNo`. The global constraints forbid a local `grammar` in a file that uses the `grammar::` namespace, and this file now does.

## TDD evidence

RED 1, with the tests written and the constants missing: `cmake --build --preset tests` failed.
```
GbnfTests.cpp(66,5): error C2039: 'GBNF_MAX_STRING_CHARS': is not a member of 'morpheus::grammar'
GbnfTests.cpp(111,5): error C2039: 'GBNF_MAX_LIST_ITEMS': is not a member of 'morpheus::grammar'
...
```
RED 2 was a runtime failure. The new constants were in place, and the old checks were kept with literal 2000 so the code compiled. Command: `./build/tests/bin/Debug/morpheus_tests.exe "[gbnf],FromSchema*,The catalog*,The grammar of every*"`
```
GbnfTests.cpp(112): FAILED: CHECK_THROWS_AS( grammar::ListOf("x", 1, grammar::GBNF_MAX_LIST_ITEMS + 1), core::ContractError ) because no exception was thrown
GbnfTests.cpp(120..127): FAILED: the 8 ListOf element-shape CHECK_THROWS_AS, no exception thrown
GbnfTests.cpp(67): FAILED: CHECK_THROWS_AS( grammar::JsonString(0, grammar::GBNF_MAX_STRING_CHARS + 1), core::ContractError )
SchemaTests.cpp: FromSchema accepted the schema {"type": "array", "items": {"const": 1}, "maxItems": 1001}
SchemaTests.cpp: FromSchema accepted the schema {"type": "string", "maxLength": 2000}
SchemaTests.cpp(82), (84): array grammars still inline: root ::= ("[]" | "[" "1" ("," "1"){0,1} "]")
SchemaTests.cpp(91): nested arrays inline
test cases:  24 |  18 passed |  6 failed
assertions: 306 | 289 passed | 17 failed
```
GREEN:
- `cmake --build --preset tests` built with no warnings or errors.
- `"[gbnf],FromSchema*,The catalog*,The grammar of every*"`: All tests passed (306 assertions in 24 test cases).
- `"[grammar]"`: the characterization tests for Morpheus's grammars passed unchanged (338 assertions in 25 test cases).
- `ctest --preset tests`: 100% tests passed out of 358.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests` built with no warnings.
- With `MORPHEUS_TEST_CHAT_MODEL=models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf MORPHEUS_TEST_EMBED_MODEL=models/nomic-embed-text-v1.5.Q8_0.gguf`, `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe` on the three LlamaGrammarTests cases: All tests passed (26 assertions in 3 test cases).
- The whole `"[llama]"` run: 28 test cases, 23 passed and 5 skipped, 100/100 assertions. The skips are the thinking-model tests (no thinking model configured) and the non-ASCII model path test, all unrelated to this change.

The old shapes failing in llama.cpp, from that run's stderr:
```
parse: error parsing grammar: number of rules that are going to be repeated multiplied by the new repetition exceeds sane defaults, ...
root ::= "\"" char{0,2000} "\""
failed to parse grammar
parse: error parsing grammar: number of rules that are going to be repeated multiplied by the new repetition exceeds sane defaults, ...
root ::= "[" "\"" char{0,48} "\"" ("," "\"" char{0,48} "\""){0,49} "]"
failed to parse grammar
```

## Escape-sequence checks

Final checks on all 7 changed files (Gbnf.cpp, Gbnf.h, Schema.cpp, Schema.h, GbnfTests.cpp, SchemaTests.cpp, LlamaGrammarTests.cpp):
- The non-ASCII grep printed nothing.
- The control-byte count printed 0 for each.

The intended escapes are still literal:
- x7F in the CHAR_RULE and in the tests;
- xC3 xA9 in GbnfTests and SchemaTests;
- u00e9 at SchemaTests.cpp:182 and GbnfTests.cpp:53.

One slip happened along the way. A Python edit script passed through the shell tool and collapsed the C++ char literal for a backslash, in ListOf's literal check, to a lone backslash. I fixed it with the Edit tool and confirmed with od that the bytes read quote, backslash, backslash, quote.

## Deviations

- I renamed the local variable `grammar` to `yesNo` in the existing `[llama]` CheckGrammar test, as described above.

## Concerns

- `GBNF_MAX_LIST_ITEMS = 1000` is conservative, as the controller decided.
  - `ListOf` always writes the element once before the repeated group (`"[" x ("," x){..}`), so the element is never first seen inside the group.
  - The group then creates one symbol, and llama.cpp's product check alone would allow `{0,1999}`, that is maxItems 2000.
  - I kept the controller's bound. The comment states the two-symbol assumption it rests on.
- The `[schema]` tag is still shared with the StateSchema tests.
