# Task 8 report: prompt assembly and budgets

Status: DONE. Commit 00700c5 "Assemble layered prompts with exact token budgets".

## Implemented
src/context/PromptBuilder.{h,cpp} (AssembleResolvePrompt, ExtendForNarrate, ContextInputs, LayerTokens, Assembly) and tests/context/PromptBuilderTests.cpp, all verbatim from the brief. PromptBuilder.cpp added to src/context/CMakeLists.txt.

## TDD evidence
RED (test file only, sources absent): `cmake --build --preset tests` ->
`tests\context\PromptBuilderTests.cpp(5,1): error C1083: Cannot open include file: 'context/PromptBuilder.h'`
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[context]"` -> "All tests passed (49 assertions in 8 test cases)".
Full: `ctest --preset tests` -> 100% tests passed out of 96.

## Files
src/context/PromptBuilder.h, src/context/PromptBuilder.cpp, src/context/CMakeLists.txt, tests/context/PromptBuilderTests.cpp

## Deviations
None. BudgetSettings field order matches the aggregate init in RoomyBudget(), so no designated initializers. No budget-section sizes needed adjusting: every section hit its intended branch on the mini world.

## Concerns
None. (I first created the sources before the RED run by mistake, so RED was produced by temporarily moving them away; code is unchanged.)

## Fix round 1

Changes (src/context/PromptBuilder.h/.cpp, tests/context/PromptBuilderTests.cpp):
- `LayerTokens` stays raw per-layer text counts; header documents `total` as the only template-inclusive figure.
- `Assembly::transcriptCost` added = `tokens.total - CountPromptTokens({system, tail})`.
- `transcriptBudget = ContextSize - resolveMax - narrateMax - tailAllowance - systemCost`, `systemCost = CountPromptTokens({system})`; `transcriptOverBudget = transcriptCost > transcriptBudget`.
- A non-positive `transcriptBudget` throws `core::DataError` ("context_size {} leaves no room for the transcript after the system layers ({} tokens), the tail allowance and both generations").
- `contextOverflow` kept as the final safety check and documented in the header.
- Removed the `std::min(tokens.summary, summaryCap)` term (and the unused `<algorithm>` include).
- The tail is moved into its message after being counted (the system+tail Prompt is built once, counted with and without the tail, then its messages are moved into the final prompt).
- Tests: roomy budget/cost values; transcript exactly at budget (fits) and one token over; contextOverflow alone (tail allowance set to the tail's raw tokens, so its 4 template tokens overflow); CHECK_FALSE exclusivity in each overrun section; degenerate budget (negative and zero throw "no room for the transcript", budget 1 does not). Old "transcript over budget" test used a 700-token context which is now degenerate, so it was replaced by the boundary sections.

Commands:
- `cmake --build --preset tests` : OK, no warnings
- `./build/tests/bin/Debug/morpheus_tests.exe "[context]"` : All tests passed (77 assertions in 9 test cases)
- `ctest --preset tests` : 100% tests passed out of 97
