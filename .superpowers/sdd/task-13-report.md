# Task 13 report: replay

Implemented turn::CallKindOf, CallKindName, ReplayStep, RecordingBackend, TurnResultJson, MakeReplayFixture
(src/turn/Replay.h/.cpp, added to src/turn/CMakeLists.txt), the hand-written fixture
tests/data/replays/mini-undo-regenerate.json and tests/turn/ReplayTests.cpp (2 test cases, tag [replay]).

TDD evidence
- RED: `cmake --build --preset tests` -> ReplayTests.cpp(17,1): error C1083: Cannot open include file: 'turn/Replay.h'.
- GREEN: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[replay]"` -> All tests passed (22 assertions in 2 test cases).
- `ctest --preset tests` -> 100% tests passed out of 186.
- `cmake --build --preset dev` -> builds clean (morpheus_tests and morpheus_probe linked).

Files: src/turn/Replay.h, src/turn/Replay.cpp, src/turn/CMakeLists.txt, tests/turn/ReplayTests.cpp,
tests/data/replays/mini-undo-regenerate.json.

Deviations: CallKindOf uses an `if (!prompt.messages.empty())` block with a const-ref `last` instead of the
ternary that bound a temporary (as the brief allowed). Added short comments in CallKindOf and Complete.
Needed a `cmake --preset tests` re-run after adding Replay.cpp to the CMake list.

Concerns: the replay test assigns `core::ParseMemoryMode(...)` (an optional) to settings.memoryMode as in the brief,
so a misspelled memory_mode in a fixture silently falls back to the world default instead of failing loudly.
Not changed (brief verbatim); could be tightened with a REQUIRE on the parse result.

## Fix round 1

Commit: eab6f0c "Make replay fixtures fail loudly and test call-kind detection"

Changes
1. memory_mode is parsed into a local optional; REQUIRE(mode.has_value()) with INFO naming the fixture and the text.
2. Steps dispatch on explicit keys input / undo / regenerate; anything else is FAIL naming the fixture and printing the step JSON.
3. New test case covering CallKindOf for all four kinds (lookup grammar, resolve grammar of the mini world, summarize and condense prompts, other prompt, empty prompt) and CallKindName for the four spellings.
4. CallKindOf now detects Lookup by the grammar's root rule: grammar.starts_with(first line of BuildLookupGrammar(), computed once into a function-local static, so it cannot drift from the builder). Header comment updated. New test case builds a real resolve grammar from an edited copy of the mini world (temp folder named "mini", character gets a string field "lookups"), asserts the grammar contains "lookups" and is still Resolve.
5. After each fixture, CHECK every reply queue except summarize is empty; after the directory loop REQUIRE(fixturesRun > 0).
6. Summary fallbacks are counted per fixture and reported with WARN when non-zero (none for the current fixture).

TDD evidence
- RED (fix 4): "[replay]" -> ReplayTests.cpp(184): CHECK( CallKindOf(Prompt{}, resolveSpec) == Resolve ) failed, 0 == 1 (substring detection called it Lookup).
  (An earlier run failed on the world loader: folder name must equal the world id; fixed by naming the temp folder "mini".)
- Negative checks of fixes 1, 2, 5 by temporarily editing the fixture (restored with git checkout): "engine " -> REQUIRE( mode.has_value() ) failed with fixture and mode text; {"redo": true} step -> FAIL "has an unknown step: {"redo":true}"; an extra narrate reply -> CHECK( queue.empty() ) failed with "unused 'narrate' replies".
- GREEN: "[replay]" -> All tests passed (39 assertions in 4 test cases). ctest --preset tests -> 100% tests passed out of 188.

Deviation: the lookups-field test uses an edited copy of the mini world (cheap), not a hand-written grammar string.
Concerns: none.
