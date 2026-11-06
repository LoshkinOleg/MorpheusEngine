# M1 hardening report

Branch `cpp`, one commit per H-section. Commands: build `cmake --build --preset tests`; focused tests
`./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`; suite `ctest --preset tests`.
Baseline before H1: 57 tests passed.

## H1. Shared int-range helper - f4b447f
Changed: new `src/core/JsonInt.h/.cpp` (`JsonIntInRange`, added to `src/core/CMakeLists.txt`; also a
`MORPHEUS_REQUIRE(lo <= hi)` precondition). Replaced all five sites: `StateSchema::FromJson` bounds,
`CheckFieldValue` Int value, `CheckFieldValue` counter count (range 1..INT_MAX), `Changes.cpp`
`IsIntRangeInteger` (deleted; int add and counter add use the helper), `World.cpp` difficulty targets. All
existing error/rejection messages kept (the int "outside lo..hi" message now prints `value.dump()`).
New tests: `tests/core/JsonIntTests.cpp` `[jsonint]` (boundaries, lo-1/hi+1, INT_MIN/INT_MAX bounds, uint64 max,
2.5, 5.0, true, "5", null, array, negatives); `[changes]` add INT_MIN to health (clamps to 0, note
"clamped to 0") and to `inventory.rope` (removed); `[schema]` bounds INT_MIN/INT_MAX accepted.
RED: `cmake --build --preset tests` ->
`JsonIntTests.cpp(8,1): error C1083: Cannot open include file: 'core/JsonInt.h'`.
GREEN: `morpheus_tests.exe "[jsonint]"` -> `All tests passed (26 assertions in 6 test cases)`;
`ctest` -> `100% tests passed out of 65`.
Note: the two negative-boundary tests (Changes, StateSchema) passed already on the old code; they are coverage
additions, not behavior changes, so they had no RED.

## H2. Fail-fast basics - e39371d
Changed: `MORPHEUS_REQUIRE` appends ` [file:line]`; `WriteFile` closes the stream and throws
`std::runtime_error` naming the path on failure; `Csv.cpp` includes `<cstddef>` and `<utility>`; both
`sqlite3_close` calls became `sqlite3_close_v2` (three sites: open-failure path, move-assign, destructor);
`ApplyChanges` documents the precondition and uses `MORPHEUS_REQUIRE` for: entity has a string `type`, type is
known, entity has a `fields` object, addressed field is present. New tests: `tests/TestPathsTests.cpp`
`[testpaths]` (WriteFile failure, REQUIRE location) and a `[changes]` case (missing `location` then
`set player.location` throws ContractError; also unknown type, no type, no fields).
RED: `[changes]` -> `[json.exception.out_of_range.403] key 'location' not found` (and 'type', 'fields').
`[testpaths]` with the old `Errors.h`/`TestPaths.h` restored -> `test cases: 2 | 2 failed`
("no exception was thrown where one was expected"; message lacked ` [`).
GREEN: `ctest` -> `100% tests passed out of 68`.

## H3. Unknown keys are errors - dcfde8a
Changed: `StateSchema::FromJson` rejects unknown keys at top level, entity type and field level (allowed field
keys depend on the type: `min`/`max` only for int, `values` only for enum); `LoadWorld` rejects unknown
world.json keys (allowed list includes `stats`, added to the data in H4). Errors are `DataError` naming the key.
Tests: `[schema]` "creatible", "defualt", "min" on string, plus top-level "entity_typs", "values" on int, "max"
on enum; `[world]` "memory_mod".
RED: `[schema],[world]` -> `test cases: 16 | 14 passed | 2 failed`, `assertions: 99 | 92 passed | 7 failed`
("no exception was thrown where one was expected").
GREEN: `ctest` -> `100% tests passed out of 69`.

## H4. Explicit stats in world.json - a0916ce
Changed: `World::stats` (`std::vector<std::string>`, after `checkEntity`, comment updated). `LoadWorld` requires
a non-empty `stats` array of unique non-empty strings (shape checked while reading world.json), then after the
schema loads checks each stat is an `int` field of the type of `initialState[check_entity]`
(`DataError` naming the stat). Data: sandcrawler `["grit","wits","presence","agility"]`, mini `["agility"]`.
`MiniWorldJsonWith` base JSON includes `stats`. Tests: mini stats, sandcrawler stats, "luck" (not a field),
"location" (not int), empty array, plus duplicate, empty-string entry and missing `stats`.
RED: `[world]` -> `test cases: 4 | 1 passed | 3 failed`, `assertions: 46 | 38 passed | 8 failed`.
GREEN: `ctest` -> `100% tests passed out of 69`.

## H5. World hash format and line endings - b7532fd
Changed: in the `LoadWorld` read loop each file is normalized (CRLF to LF) before hashing and parsing; hash
per file = name, "\n", decimal length, "\n", text (documented in a comment). `.gitattributes` added with the two
`eol=lf` rules; `git add --renormalize game_projects tests/data` produced no changes (the committed blobs were
already LF; `git ls-files --eol` shows `i/lf` for all). Tests: CRLF copy of the mini world (written with
`\r\n` via WriteFile) hashes and loads equal to the LF copy, `world.rules` and lore have no `\r`; an
independent re-implementation of the documented hash format matches `contentHash`; ContentHash 0xFF
and `Add("")`.
0xFF reference: independently computed `(0xcbf29ce484222325 ^ 0xff) * 0x100000001b3 mod 2^64 = af64724c8602eb6e`,
equal to the brief's value.
RED: `[world],[hash]` -> format test `"722c22821b78bcb8" == "3cac1058e9c3047c"`, CRLF test
`"9ce1f187737f93b4" == "722c22821b78bcb8"` and `crlf.rules.find('\r')` found at 36; `test cases: 10 | 8 passed
| 2 failed`. (The two ContentHash tests passed immediately: they pin existing behavior.)
GREEN: `ctest` -> `100% tests passed out of 73`.

## H6. Dice - f39df8f
Changed: `CheckSeed` mixes `SplitMix64(runSeed).Next()` first, then
`SplitMix64(base ^ ((turn << 32) | checkIndex))`; `CheckResult::total` is `long long` and `RollCheck`
accumulates in it. Tests: seed-collision test (old code collides for `(A,1)` vs `(A ^ (3<<32), 2)`), success
boundary (total == target succeeds, target = total+1 fails, same seed), INT_MAX stat (`total > INT_MAX`), and
pinned golden values:
`CheckSeed(42,1,0) == 0x8a5509b0d5741cc5`; `RollCheck(2d6+stat, 3, 10, that seed)` rolls `{1, 5}`, total 9,
failure. Both cross-checked with an independent Python SplitMix64 implementation.
RED: `[dice]` -> `test cases: 9 | 7 passed | 2 failed`, `assertions: 638 | 633 passed | 5 failed`
(`-2147483640 > 2147483647` overflow; seed collision `0xf40c5155cd3742b6 != 0xf40c5155cd3742b6`).
GREEN: `ctest` -> `100% tests passed out of 77`.

## H7. Notes committed with their turn - 9b58789
Changed: `TurnRecord::notes`; `CommitTurn` inserts notes with `turn = record.number` inside the same
transaction; `Turn()`/`Turns()` fill `notes` ordered by id (one extra query per call; an orphan note row is a
`StoreError` naming the file, as damaged saved data); public `AddNote` removed; `Notes()` kept; doc comments
updated. Existing tests (including TruncateAfter and summaries/notes order) now pass notes via `CommitTurn`;
`TestTurn` got an optional `notes` parameter. New test: failed `CommitTurn` (wrong and duplicate turn number)
leaves no notes.
RED: build error `RunStoreTests.cpp(210,12): error C2039: 'notes': is not a member of ...TurnRecord`.
GREEN: `ctest` -> `100% tests passed out of 78`.

## H8. Embedding content hash - b6399f4
Changed: `content_hash TEXT NOT NULL` in `SCHEMA_SQL`; `EmbeddingRow::contentHash` (documented);
`PutEmbedding` requires it non-empty (`MORPHEUS_REQUIRE`) and stores it; `Embeddings()` returns it. Tests
updated to set and check the hash; a replace changes the stored hash (`hash-old` -> `hash-new`); empty hash
and missing hash throw ContractError.
RED: build error `RunStoreTests.cpp(347,10): error C2664: ... cannot convert argument 1 from 'initializer list'`
(src changes stashed, tests kept).
GREEN: `ctest` -> `100% tests passed out of 78`.

## Mechanical adjustments and observations
- `JsonIntInRange` also requires `lo <= hi` (ContractError); the brief did not mention it.
- `Changes.cpp` has two constants `INT_MIN_VALUE` / `INT_MAX_VALUE` (the names `INT_MIN`/`INT_MAX` are macros).
- `src/core/World.cpp`: lone `\r` (not followed by `\n`) is kept as written; only CRLF pairs are converted.
- H3 allowed `stats` in the world.json key set from the start, so sandcrawler/mini kept loading before H4.
- `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md` showed as modified in the working
  tree from the second commit on; it was edited outside this round (not by me) and was never staged.
- `git config core.autocrlf` is `true` here, so git prints "LF will be replaced by CRLF" warnings. The
  working-tree copy of `game_projects/sandcrawler/lore.csv` is CRLF while its blob is LF; loading is
  unaffected because of the H5 normalization.

## Final result
`ctest --preset tests`: `100% tests passed, 0 tests failed out of 78` (`Total Test time (real) = 1.73 sec`).
`morpheus_tests.exe`: `All tests passed (1000 assertions in 78 test cases)`.
