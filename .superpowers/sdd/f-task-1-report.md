# Sub-project F, Task 1: turn::PromptPack (report)

## Implemented
- `src/turn/PromptPack.h`, `src/turn/PromptPack.cpp`: `PromptVariables`, `PromptText`, `PROMPT_TEXTS` (six entries in table order), `PromptPack` with the six text members (defaulted to the EngineText HOSTED_* constants), `FromHost` (checks: unknown keys in key order, then each text in table order: empty, then unknown variables in text order), `Fill`, `Text`.
- `src/turn/CMakeLists.txt`: `PromptPack.cpp` added after `MemoryUpkeep.cpp`.
- `src/turn/EngineText.h`: comments only; values unchanged. Each hosted constant's comment names its pack key.
- `tests/turn/PromptPackTests.cpp`: four `[prompts]` test cases (defaults, host replacement, fault messages, literal braces, Fill).

## TDD evidence
- RED: `cmake --build --preset tests` failed with `PromptPackTests.cpp(8,1): error C1083: Cannot open include file: 'turn/PromptPack.h'`.
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"` -> `All tests passed (49 assertions in 5 test cases)`.
- Full suite: `ctest --preset tests` -> `100% tests passed out of 658`; 6 `[server]` tests skipped (no real llama-servers configured).
- The test build showed no warnings from the new files under /W4 /WX.

## Deviations from the brief (mechanical)
1. `tests/turn/PromptPackTests.cpp`: `INFO(text.key)` changed to `INFO(std::string(text.key))`. Catch2 in this build has no stringify for `std::string_view` (link error LNK2019 on `StringMaker<string_view>::convert`).
2. `tests/turn/PromptPackTests.cpp`: comparisons against `turn::HOSTED_*` (string_view constants) wrapped as `std::string(turn::HOSTED_...)`, same convention as `tests/context/HostedPromptTests.cpp`. Same cause as item 1 (`std::string == string_view` operands could not be stringified for the failure message).
3. `src/turn/PromptPack.cpp`: the unknown-key loop uses `for (const auto& entry : texts)` with `entry.first` instead of a structured binding, so the unused `text` binding does not raise a warning. The lambda parameter is named `candidate` rather than `known`, to avoid shadowing the `known` bool being declared. Behaviour is unchanged.
4. `src/turn/PromptPack.cpp`: `MEMBERS` array formatted across lines differently from the brief; content identical.

## Environment issue
- A build I stopped mid-run (via TaskStop) left orphaned `MSBuild`/`link` processes holding `vc143.pdb` (C1041). Killing them was denied by the auto-mode permission classifier, so I did not force it. The stale processes exited on their own and the rebuild then succeeded. The user may want to check that no stale MSBuild/link processes remain.

## Files changed (committed in 5c59cc9)
- src/turn/PromptPack.h (new)
- src/turn/PromptPack.cpp (new)
- src/turn/CMakeLists.txt
- src/turn/EngineText.h (comments only)
- tests/turn/PromptPackTests.cpp (new)

## Concerns
- None blocking. Untouched and not staged: `.superpowers/sdd/progress.md`, `docs/references/*` submodules, `settings.json`, `models/`.
