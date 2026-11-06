# Task 1 report: core carry-overs

Status: DONE

## Implemented
- core/Text.h/.cpp: PathText, SHORT_JSON_CHARS, ShortJson, CutUtf8, Trim (verbatim from brief); Text.cpp added to CMake.
- Changes: over-removal rejected ("only N 'item' held, cannot remove M"), exactly 0 removes with note "none left", ShortJson in the 3 rejection reasons; Changes.h comment updated.
- StateSchema::CheckFieldValue: value/item/count dump() -> ShortJson (the enum `def.values` list, world data, left as is).
- Dice: CallKind, SamplingSeed, SAMPLING_SALT.
- UTF-8 paths: Settings.cpp, World.cpp (incl. folderName), RunStore.cpp, Sqlite.cpp use PathText; requireModel uses non-throwing is_regular_file; RunStore::Create/Open use error_code overloads with the specified StoreError messages; LlamaCppBackend Utf8 helper removed, uses core::PathText (model load still passes u8string bytes).

## TDD
RED: `cmake --build --preset tests` failed: "Cannot open include file: 'core/Text.h'", 'SamplingSeed' identifier not found, 'CallKind' not a class or namespace.
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[text],[changes],[dice]"` -> All tests passed (755 assertions in 32 test cases).
Full: `ctest --preset tests` -> 100% tests passed out of 106.
`cmake --build --preset dev` -> succeeded (llama backend, probe, tests compile). [llama] tests not run.

## Files changed
src/core: Text.h, Text.cpp (new), CMakeLists.txt, Changes.h, Changes.cpp, StateSchema.cpp, Dice.h, Dice.cpp, Settings.cpp, World.cpp; src/store: RunStore.cpp, Sqlite.cpp; src/llama/LlamaCppBackend.cpp; tests/core: TextTests.cpp (new), ChangesTests.cpp, DiceTests.cpp.

## Deviations
- Existing test "add accepts INT_MIN as a negative delta" asserted that removing INT_MIN ropes (1 held) succeeds and empties the inventory. That contradicts the new over-removal rule, so I updated it to expect a rejection containing "only 1 'rope' held, cannot remove 2147483648" (also shows -delta does not overflow). Comment adjusted.

## Concerns
None.
