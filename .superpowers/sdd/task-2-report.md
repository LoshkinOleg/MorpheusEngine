# Task 2 report: settings for the turn layer

Status: DONE

## Implemented
- src/core/MemoryMode.{h,cpp}: MemoryMode enum moved out of World.h, plus ParseMemoryMode; added to core CMakeLists.
- World.h includes MemoryMode.h; World.cpp uses ParseMemoryMode.
- Settings: BudgetSettings::retrievalCap (last member, 1..tail_allowance, required), Settings::memoryMode (optional key),
  SETTINGS_TEMPLATE_MARGIN = 64 in the room check, SettingsToJson (PathText for model paths).
- settings.example.json and the local (unstaged) settings.json gained "retrieval_cap": 768.
- Tests: ValidSettingsJson gets retrieval_cap; four new test cases from the brief.

## TDD evidence
- RED: `cmake --build --preset tests` -> `error C1083: Cannot open include file: 'core/MemoryMode.h'` (SettingsTests.cpp).
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[settings],[world]"` -> All tests passed (95 assertions in 12 test cases).
- `ctest --preset tests` -> 100% tests passed out of 110.
- Local settings.json: built `morpheus_probe` (dev preset) and ran it with settings.json; it loaded and ran.

## Files changed
src/core/{MemoryMode.h,MemoryMode.cpp,CMakeLists.txt,World.h,World.cpp,Settings.h,Settings.cpp}, tests/core/SettingsTests.cpp, settings.example.json. (settings.json modified, not staged.)

## Deviations
None. World.h has CRLF in the working tree (LF in the index); my edit preserved CRLF.

## Concerns
None. tools/probe and context/PromptBuilder use settings but compile unchanged.

## Fix round 1

Commit: e5e590c "Make stored model paths absolute and show the template margin" (files: src/core/Settings.cpp, tests/core/SettingsTests.cpp).

### What changed
- Important: `LoadSettings`'s `requireModel` now makes the joined model path absolute with `std::filesystem::absolute(path, error)`. An error throws a `DataError` naming the key and the file. A settings file loaded by a relative path (for example `LoadSettings("settings.json")`) now stores absolute model paths, so `SettingsToJson` matches its header comment.
- Minor: the "no room for the transcript" message now names the margin: "budget uses {} of {} context tokens (plus a {}-token chat-template margin) and leaves no room for the transcript". The substring "no room for the transcript" is kept.
- Tests: new case "LoadSettings makes model paths absolute for a relative settings path". It switches into the folder's parent through a RAII guard (`CurrentPathGuard`) that restores the old current path, loads `"<folder>/settings.json"`, and checks `is_absolute()` on the model paths and on `SettingsToJson(...).at("chat_model")`. The margin case gained a check on the new message text.

### RED
Command: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings]"`
Build passed. Test run:
```
SettingsTests.cpp(181): FAILED: CHECK_THROWS_WITH(... ContainsSubstring("11008 of 11038 context tokens (plus a 64-token chat-template margin)"))
  actual: "... budget uses 11008 of 11038 context tokens and leaves no room for the transcript"
SettingsTests.cpp(216): FAILED: CHECK( settings.chatModel.is_absolute() )  with expansion: false
SettingsTests.cpp(217): FAILED: CHECK( settings.embeddingModel.is_absolute() )  with expansion: false
SettingsTests.cpp(221): FAILED: CHECK( storedChat.is_absolute() )  with expansion: false
test cases: 7 | 5 passed | 2 failed
assertions: 42 | 38 passed | 4 failed
```

### GREEN
Command: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings]"`
```
Filters: [settings]
All tests passed (42 assertions in 7 test cases)
```

### Full suite
Command: `ctest --preset tests`
```
100% tests passed out of 111
Total Test time (real) =   2.39 sec
```

### Notes
- Only the two named files are in the commit. `settings.json` is untouched and unstaged.
- The `absolute` call normalizes the path only against the current working directory. It does not remove `..` segments.
