# Task 6 report: turn module and resolve grammar

Status: DONE_WITH_CONCERNS

## Implemented
- New module `morpheus_turn` (`src/turn/CMakeLists.txt`, `ResolveGrammar.h/.cpp`) exactly as in the brief:
  constants, `BuildResolveGrammar`, `BuildLookupGrammar`.
- Root `CMakeLists.txt`: `add_subdirectory(src/turn)` after context. `tests/CMakeLists.txt`: `morpheus_turn` linked into `morpheus_tests`.
- Tests: `tests/turn/ResolveGrammarTests.cpp` ([grammar], 3 cases), `tests/llama/LlamaGrammarTests.cpp` ([llama], with `#include <algorithm>` added).

## TDD evidence
- RED: header temporarily moved away; `cmake --build --preset tests` failed:
  `ResolveGrammar.cpp(1,1): error C1083: Cannot open include file: 'turn/ResolveGrammar.h'`
  (the test files were already in place; the missing header is the brief's expected failure).
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"` -> All tests passed (15 assertions in 3 test cases).
- `[llama]` (dev preset, models exported): All tests passed (70 assertions in 19 test cases). llama.cpp accepted
  both grammars as generated: no generator change was needed. (The "error parsing grammar ... root ::= (" log line
  in that run comes from the existing intentional "rejects requests that cannot work" test, not from this task.)
- `ctest --preset tests`: 100% tests passed out of 123.

## WARN line (sandcrawler, Engine mode, Qwen3-4B Q4_K_M, Vulkan)
```
resolve: 129 tokens in 70446 ms (1.8 tok/s); output: {"intent":"action","checks":[{"stat":"agility","difficulty":"medium","stakes":"climbing onto the crawler's roof","on_success":[{"op":"add","path":"player.conditions","value":"on_roof","reason":"Successfully climbed onto the crawler's roof to scan the horizon"}],"on_failure":[{"op":"add","path":"player.conditions","value":"failed_climb","reason":"Failed to climb due to low agility and instability"}]}],"changes":[{"op":"add","path":"player.conditions","value":"on_roof","reason":"Player successfully climbed onto the crawler's roof to scan the horizon"}]}
```
Another run: 129 tokens in 110383 ms (1.2 tok/s). The test takes 70-110 s.

## Concern: grammar-constrained decode is very slow
- Same prompt and settings with no grammar: 114 tokens in 5258 ms (21.7 tok/s). Constrained is roughly 12-18x slower.
- Not caused by the bounded repetitions: I temporarily replaced `char{0,120}` / `char{0,400}` with `char*`; the speed was identical (1.8 tok/s). Reverted.
- Likely cause (not verified): the `change` rule has one alternative per field of every live entity (plus creates), and it
  appears in three places (branches and unconditional changes), so grammar sampling keeps many parallel stacks.
  Possible levers if this matters: factor common prefixes (op/path) so alternatives share a prefix, or cut the stack count
  some other way. That would change the grammar design, so I did not attempt it.
- The existing hand-written "follows a GBNF grammar" test runs in about 1 s, so the slowdown is specific to this grammar's size.

## Deviations
- None to the interfaces or values. Only change beyond the brief text: `#include <algorithm>` in the llama test (brief asked for it).
- The file edits to the temporary experiments (char*, empty grammar) were reverted before the commit.

## Files
CMakeLists.txt, tests/CMakeLists.txt, src/turn/CMakeLists.txt, src/turn/ResolveGrammar.h, src/turn/ResolveGrammar.cpp,
tests/turn/ResolveGrammarTests.cpp, tests/llama/LlamaGrammarTests.cpp

## Fix round 1

Commit: ccd65df "Match held counter items in the grammar and test the grammar's structure" (four files: ResolveGrammar.h/.cpp, ResolveGrammarTests.cpp, LlamaGrammarTests.cpp).

### What changed, item by item
1. **Held counter items.** For each counter field of each live entity, the path's item part is a per-field rule `items-N ::= "<held 1>" | "<held 2>" | item` (held names in the entity's key order), or plain `item` when nothing is held. Each held name is `nlohmann::json(name).dump()` with the quotes stripped, then `Literal()`. The add and remove alternatives both use the rule. The header comment now says new items use `[a-z0-9_]{1,MAX_ITEM_CHARS}` and held items are matched by name. Tests: "Rusty Knife", `water-flask` and `say "hi"` (checks double escaping) with the generic `item` alternative kept; nothing held gives `item` and no `items-` rule; the existing mini-world test now finds `items-N` by its right-hand side.
2. **Unit tests.** In `tests/turn/ResolveGrammarTests.cpp`:
   - A test-local `GrammarProblem()` checks every line is `name ::= rhs`, every rule is defined exactly once, `root` exists, every referenced rule is defined, and quotes, brackets and parentheses balance. It skips string literals and character classes with escapes, and `{n,m}` counts. A test shows it catches an undefined rule, a double definition, an unbalanced parenthesis and an unbalanced quote.
   - It is applied to the engine, assisted, lookup, held-item, escaped-enum, list/enum and empty grammars.
   - New tests: list field add/remove; enum `set` on an existing entity; an enum value `a"b\c` (expected literal `"\"a\\\"b\\c\""`); enum rule sharing (the live npc and a create reference the same rule, found by searching, and the `"wary"` rule is defined once); list bounds (`{0,2}` checks, `{0,5}` changes, `{0,3}` branch, `{0,1}` notes, `{0,2}` lookups); `ContractError` for array and null state. The existing `enum-0` assertion now searches for the name.
3. **Nothing to change.** The `REQUIRE(!changes.empty())` is gone. With no changes, `changes ::= "[]"` and `branch ::= "[]"`, and no `change` rule is emitted. The loader requires non-empty `fields` for every type, so a world cannot reach this through a type without fields. It is reachable with an empty state (no entities) and no creatable type, and the test uses that (a legal schema, state `{}`). The header documents it.
4. **Tidy-ups.** `typeName` is hoisted. The constants block comments are aligned and every constant has one. A `static_assert` covers MAX_CHECKS, MAX_CHANGES, MAX_BRANCH_CHANGES, MAX_NOTES, MAX_LOOKUPS and MAX_LIST_ITEMS being >= 2, placed in the header next to the constants. The runtime REQUIRE in `ListOf` stays.
5. **Model-backed test.**
   - `maxTokens` is 1024.
   - Every check's `stat` must be in `world.stats` and its `difficulty` a key of `world.difficulties`.
   - `ParseChange` runs on the unconditional changes plus every `on_success` and `on_failure` entry, and `REQUIRE`s that it parses.
   - `ApplyChanges` then runs on those entries together against the initial state. Rejections are `WARN`ed, not failed.
   - The speed `WARN` is kept.

### RED
`cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"` before any source change: 11 test cases, 8 passed, 3 failed (39 assertions, 3 failed):
- "The resolve grammar names the world's stats, difficulties and field shapes": `REQUIRE_FALSE( items.empty() )` failed (no `items-N` rule).
- "Held counter items are matched by name next to the generic item rule": `REQUIRE_FALSE( items.empty() )` failed.
- "A state with nothing to change ...": `BuildResolveGrammar: the state has no fields to change [ResolveGrammar.cpp:152]`.
The other new tests (list, enum set, escaping, sharing, bounds, structure, ContractError) cover existing behavior and passed at once.

### GREEN
- `./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`: All tests passed (54 assertions in 11 test cases).
- `ctest --preset tests`: 100% tests passed out of 131.
- `[llama]` under `dev` (models exported in the same command), `morpheus_tests.exe "[llama]"`: All tests passed (86 assertions in 20 test cases). The stderr lines "parse: error parsing grammar: expecting ')'" and "state_seq_set_data: error loading state" come from other tests that check those failures.

### WARN lines from the [llama] grammar test
Only the speed line (no "rejected on apply" lines, so every proposed change applied cleanly):
```
resolve: 129 tokens in 10137 ms (12.7 tok/s); output: {"intent":"action","checks":[{"stat":"agility","difficulty":"medium","stakes":"climbing onto the crawler's roof","on_success":[{"op":"add","path":"player.conditions","value":"on_roof","reason":"Successfully climbed onto the crawler's roof to scan the horizon"}],"on_failure":[{"op":"add","path":"player.conditions","value":"failed_climb","reason":"Failed to climb due to low agility and instability"}]}],"changes":[{"op":"add","path":"player.conditions","value":"on_roof","reason":"Player successfully climbed onto the crawler's roof to scan the horizon"}]}
```
Constrained decode is now 12.7 tok/s, up from 1.2-1.8 tok/s in the original report (the d6ed70f sampling change).

### Deviations and notes
- No interface changes. The sandcrawler world holds no counter items, so the model-backed run does not exercise the held-item rule. Unit tests cover it.
