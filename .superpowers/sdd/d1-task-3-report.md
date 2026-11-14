# D1 Task 3 report: the shared memory upkeep

Status: DONE

Commit: 5d161f9 Move Session's fit loop, fold and condense into turn::MemoryUpkeep, working on rendered transcript entries

## What was implemented

All per the brief, verbatim except the deviations listed below.

- `context::TranscriptEntry` and `context::TranscriptEntries(turns, proseTurns)` (src/context/Transcript.h/.cpp). `messages` comes from `RenderTranscript(turns, proseTurns)`, `foldMessages` from `RenderTranscript(turns, INT_MAX)`, `fact` is the input plus " " plus the mechanics line when there is one.
- `context::ContextInputs::transcript` is now `std::vector<TranscriptEntry>`, and `proseTurns` is gone. `AssembleResolvePrompt` pushes each entry's messages and counts their tokens exactly as before.
- `turn/Folding.h/.cpp`: `SummarizePrompt`, `SummarizeFallback` and `SummarizeTurns` take entries. The condense functions are unchanged.
- New `turn/MemoryUpkeep.h/.cpp`:
  - `TurnError`, `FOLD_SUMMARY_DIVISOR` and `CONDENSE_SUMMARY_DIVISOR` moved here from Session.h.
  - New: `FitPass`, and `MemoryUpkeep` with `Fit`, `Settle`, `FoldAll`, a private `FoldOldest(..., wanted, ...)` and a private `Condense`.
  - `CondensedOverCap` moved here.
  - Added to src/turn/CMakeLists.txt.
- `Session`:
  - `Inputs`, `AssembleFitting`, `FoldOldest` and `Condense` are replaced by `Pass` and `Upkeep`.
  - `Maintain` calls `Upkeep(latest, 0).Settle(...)`, then `SyncHistory` as before.
  - Both fit calls in `PlayTurn` go through `Upkeep(turn, attempt).Fit(...)`.
  - The public API is unchanged. Session.h includes turn/MemoryUpkeep.h.
  - The check `grep -n "AssembleFitting\|FoldOldest\|Inputs(\|CondensedOverCap" src/turn/Session.h src/turn/Session.cpp` prints nothing.

### Why the behaviour is the same
- **Tail message:** "the input and the state" + " use N tokens, over tail_allowance M" gives the same text as before.
- **Fold prompt:** each entry's `foldMessages` are exactly `RenderTranscript(turns, INT_MAX)`, which the old `SummarizePrompt` used.
- **Fallback summary:** the fallback text is "Turn N: input[ mechanics]", as before.
- **Seeds and caps:** the fit loop passes half the transcript, at least 1, which matches the old FoldOldest. The seeds use the same turn, attempt and `CallKind::Summarize`, and the caps are unchanged.
- **After a fold:** StoreFold does `AddSummary` plus `SyncHistory(toTurn)`, as the old FoldOldest did.

No `Session` test, replay fixture or prompt text was changed.

## TDD evidence

RED: `cmake --build --preset tests` (after writing tests only) failed with, among others:
```
tests\turn\MemoryUpkeepTests.cpp(20,1): error C1083: Cannot open include file: 'turn/MemoryUpkeep.h': No such file or directory
tests\app\ModelSeatTests.cpp(96,5): error C2039: 'TranscriptEntries': is not a member of 'morpheus::context'
tests\context\PromptBuilderTests.cpp(162,35): error C2039: 'messages': is not a member of 'morpheus::core::TurnRecord'
tests\context\RenderTests.cpp(195,32): error C2039: 'TranscriptEntry': is not a member of 'morpheus::context'
```

GREEN:
- `cmake --build --preset tests`: no warnings, no errors.
- `./build/tests/bin/Debug/morpheus_tests.exe "[upkeep],[folding],[context],[session],[retrieval],[replay]"`: `All tests passed (1261 assertions in 92 test cases)`.
- `ctest --preset tests`: `100% tests passed out of 453` (446 + 6 `[upkeep]` + 1 TranscriptEntries test). The 3 `[server]` tests were skipped.
- `cmake --build --preset asan` then `ctest --preset asan`: `100% tests passed out of 453`, with the same 3 skipped.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_play morpheus_probe morpheus_tests`: exit 0, no warnings or errors.
- `[llama]` with the Qwen3-4B and nomic models: `test cases: 33 | 27 passed | 6 skipped; assertions: 5000 | 5000 passed`. The skips are environmental:
  - three need MORPHEUS_TEST_THINKING_MODEL;
  - one needs the [server] URLs;
  - one could not hard-link the models into the temp folder;
  - one more is of the same kinds.
- `tasklist | grep -i -E "morpheus|ctest"`: nothing running.

## Byte checks (all 16 committed files)
- `LC_ALL=C grep -c $'[\x80-\xFF]' $FILES`: every file 0.
- `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES`: every file 0.
- `grep -nF -e '\u' -e '\x'` over the files: no matches.
- `grep -nF '\n'` shows these are still the literal backslash sequences:
  - Folding.cpp lines 37 and 81 (`"\n"`);
  - Session.cpp `"\n\n"` (ASSISTED_RULES);
  - FoldingTests.cpp `"  \n\t "`.

## Files changed
- src/context/Transcript.h
- src/context/Transcript.cpp
- src/context/PromptBuilder.h
- src/context/PromptBuilder.cpp
- src/turn/Folding.h
- src/turn/Folding.cpp
- src/turn/MemoryUpkeep.h (new)
- src/turn/MemoryUpkeep.cpp (new)
- src/turn/Session.h
- src/turn/Session.cpp
- src/turn/CMakeLists.txt
- tests/turn/MemoryUpkeepTests.cpp (new)
- tests/context/RenderTests.cpp
- tests/context/PromptBuilderTests.cpp
- tests/turn/FoldingTests.cpp
- tests/app/ModelSeatTests.cpp

## Deviations (mechanical)
1. **tests/turn/MemoryUpkeepTests.cpp, added header.** I added `#include <catch2/matchers/catch_matchers.hpp>`. `CHECK_THROWS_WITH` is not defined by `catch_test_macros.hpp` alone (error C3861).
2. **tests/turn/MemoryUpkeepTests.cpp, wrapped constants.** In the two `CHECK`s that compare against `turn::SUMMARIZE_INSTRUCTION` and `turn::CONDENSE_INSTRUCTION`, the constant is wrapped in `std::string(...)`.
   - Why: comparing a string with a `string_view` inside `CHECK` makes Catch2 stringify a `std::string_view`, and this Catch2 build has no `StringMaker<std::string_view>` (LNK2019).
   - The existing tests already follow this convention (FoldingTests).
   - The responder lambda's comparison is outside `CHECK`, so it was left unchanged.
3. **No doubled blank line in Transcript.h/.cpp.** The brief's insertions before `} // namespace morpheus::context` start with a blank line. The files already had one there, so I kept a single blank line.

## Concerns
None.
