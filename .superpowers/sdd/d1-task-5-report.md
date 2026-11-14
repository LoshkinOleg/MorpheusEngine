# Task 5 report: hosted prompt layers and default hosted texts

Commit: 6edc53d "Add the hosted prompt layers (context::AssembleHostedPrompt) and the default hosted texts"

## Implemented
- `context::HostedInputs`, `context::AssembleHostedPrompt` in `src/context/PromptBuilder.{h,cpp}`; the owned and hosted builders share an anonymous-namespace `LayOut` helper (counts, stable prefix, budgets, flags). Code verbatim from the brief.
- `HOSTED_SYSTEM_TEXT`, `HOSTED_RESOLVE_INSTRUCTION`, `HOSTED_SPEAK_INSTRUCTION`, `HOSTED_TURN_CUE` in `src/turn/EngineText.h`.
- `tests/context/HostedPromptTests.cpp`: 6 new test cases.

## TDD
- RED: `cmake --build --preset tests` failed: `error C2039: 'HostedInputs': is not a member of 'morpheus::context'` (and following syntax errors).
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[context]"`: All tests passed (224 assertions in 27 test cases). Owned context/turn tests unedited.
- `ctest --preset tests`: 100% passed out of 472 (466 + 6), 3 [server] skipped.
- `ctest --preset asan`: 100% passed out of 472, 3 skipped.
- Byte checks (high bytes, control bytes) on all four files: 0 for each.
- `tasklist | grep -i -E "morpheus|ctest"` after the commit: nothing running.

## Deviations
- Test file: `CHECK(turn::HOSTED_TURN_CUE == "({speaker} speaks.)")` failed to link (Catch2 has no string_view StringMaker in this build: LNK2019). Wrapped in double parentheses, `CHECK((... == ...))`, so Catch does not decompose it. Behavior unchanged.

## Concerns
- None. (Git prints LF->CRLF warnings on commit; normal here.)

## Fix round 1
- Extracted RenderAlwaysOnLore and RenderSummaries (anonymous namespace beside LayOut), used by both builders; LayOut now MORPHEUS_REQUIREs a fresh assembly; "tried" -> "requested" in HOSTED_SYSTEM_TEXT (no test pinned it); Settings.h worldCap/tailAllowance comments cover hosted; test uses `CHECK(std::string(turn::HOSTED_TURN_CUE) == ...)`.
- Byte checks: 0 for all four files. `ctest --preset tests`: 100% passed out of 472. `ctest --preset asan`: 100% passed out of 472. Existing tests unedited. tasklist: nothing running.
