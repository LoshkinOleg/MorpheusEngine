# Task 5 report

Status: DONE (one mechanical test adjustment, noted below)

## Implemented
- `StateSchema::EntityTypes()` accessor (header + cpp).
- `context::RenderWorldReference` (new `WorldReference.h/.cpp`, added to `src/context/CMakeLists.txt`), verbatim from the brief.
- `PromptBuilder.cpp`: L1 is rules, then `RenderWorldReference`, then always-on lore. `transcriptBudget` counts the summary at most at `summary_cap` (`summaryExcess`), so an over-cap summary is only the `summaryOverBudget` flag. The no-room `DataError` is kept and names `systemCost - summaryExcess`. `PromptBuilder.h` comments updated as the brief says.
- `Transcript`: `RenderChangePart` extracted from `RenderMechanicsLine` (declared in the header, which now refers to it for the change forms).
- Tests: three new `RenderTests.cpp` cases, two new `PromptBuilderTests.cpp` cases.

## TDD evidence
- RED for the summary flag: with `summaryExcess` neutralised (`0 * std::max(...)`), `./build/tests/bin/Debug/morpheus_tests.exe "An over-cap*"` failed with the DataError "context_size 1336 leaves no room for the transcript after the system layers (1113 tokens) ...". Restored afterwards.
- I did not capture a RED build for the missing `WorldReference.h` / `RenderChangePart`: I wrote the sources before the first build, so the compile-failure step was skipped. The other new tests were first run against the finished implementation.
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[context]"` gave "All tests passed (107 assertions in 16 test cases)". `ctest --preset tests` gave "100% tests passed out of 120".

## Existing PromptBuilder tests
None needed changes. They derive costs from the backend (`Measure`, `CountTokens`) and assert only substrings of the system message, so the reference block did not break any of them. No expectation was edited.

## Deviations
- The brief's test "An over-cap summary is a flag even when it leaves the transcript no room" failed on `CHECK(assembly.contextOverflow)` as written, because the tail (about 142 tokens) is far below `tailAllowance` 512, which leaves slack. I made `budget` non-const and set `budget.tailAllowance = Measure(world, inputs, budget).tail` before measuring, the same pattern the "only the context overflows" section uses. Intent is unchanged: summary flag set, contextOverflow set, transcriptBudget positive, no DataError.

## Probe (dev build, sandcrawler)
```
prompt: 723 tokens (engine rules 16, world 552, tail 142; transcript 0 of 11968 budget); fits: yes
[prompt 723 tokens: 0 reused, 723 evaluated in 367 ms; 115 generated in 4477 ms]
[prompt 723 tokens: 722 reused, 1 evaluated in 183 ms; 119 generated in 6030 ms]
```
The sandcrawler reference is in the 552 world tokens. Run 2 reuses 722 of 723 tokens.

## Files changed
- src/context/WorldReference.h, src/context/WorldReference.cpp (new)
- src/context/CMakeLists.txt, PromptBuilder.h, PromptBuilder.cpp, Transcript.h, Transcript.cpp
- src/core/StateSchema.h, StateSchema.cpp
- tests/context/RenderTests.cpp, tests/context/PromptBuilderTests.cpp

## Commit
181b318 Add the world reference to L1 and flag over-cap summaries

## Concerns
None. The `<utility>` include in `Transcript.cpp` stays because `RenderTranscript` still uses `std::move`.
