# Task 7 report

Status: DONE. Commit 737518d "Read resolve output, roll and apply it, and write the narrate message".

Implemented verbatim from the brief: src/turn/EngineText.h, Resolve.h, Resolve.cpp, tests/turn/ResolveTests.cpp; Resolve.cpp added to src/turn/CMakeLists.txt.

TDD:
- RED: with src/turn/Resolve.h absent, `cmake --build --preset tests` fails: "Cannot open include file: 'turn/Resolve.h'".
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[resolve]"` -> All tests passed (40 assertions in 6 test cases). `ctest --preset tests` -> 100% passed, 137 tests.

Deviations: none. Build was clean under /W4 /WX with no edits to the brief's code.
Concerns: none. (Non-ASCII check by grep was inconclusive on this locale, but the brief text is ASCII.)

## Fix round 1

Commit 3a744aa "Shed intent and stakes to fit the narrate message, and test the shedding".

Changes (src/turn/Resolve.h, src/turn/Resolve.cpp, tests/turn/ResolveTests.cpp):
1. RenderNarrateMessage sheds in order, each step only while over the allowance: Rejected line, Applied entries from the end, every check's ". Stakes: ..." part, then the intent cut with core::CutUtf8 to INTENT_SHORT_CHARS (40, a constexpr at the top of Resolve.h) plus "..." (added only if something was cut). DataError only when all of that still does not fit. Header comment lists the full order.
2. New test: sheds applied entries from the end (3 long applied changes): "; and 1 more" and "Applied: and 3 more".
3. Resolve.cpp includes <string>, <utility>, <vector> (and core/Text.h for CutUtf8).
4. ParseLookupOutput clears `error` first; test passes a non-empty string with valid output.
5. Top-to-bottom comments in ParseResolveOutput and ParseLookupOutput.
6. ApplyProposal: MORPHEUS_REQUIRE that the check entity's fields hold the stat as an integer, before the read.
7. Tests: unknown difficulty rejection (changes dropped, raw has stakes); stat value 0 omits "+N" (agility 0 in a copy of the mini initial state).
Also a test for stakes shed alone (intent kept whole), then stakes + intent cut ("Intent: iiii...(40)..." and fits), then DataError for allowance 5.

TDD evidence:
- RED (`cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "[resolve]"` with the new tests and the old Resolve.cpp): 2 failed.
  - "sheds stakes, then cuts the intent": `narrate_allowance 120 is too small for the resolution record (170 tokens)`
  - "ParseLookupOutput clears a stale error": `CHECK( error.empty() )` false
  The applied-shedding, unknown-difficulty and stat-0 tests passed immediately: they cover existing behavior that was untested (characterization tests), so they have no RED.
- GREEN: `morpheus_tests.exe "[resolve]"` -> All tests passed (64 assertions in 11 test cases). `ctest --preset tests` -> 100% passed, 142 tests.

Notes: build clean under /W4 /WX. Nothing beyond the brief added.
