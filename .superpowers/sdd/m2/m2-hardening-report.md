# M2 hardening report

Status: DONE_WITH_CONCERNS. Three commits, one per section.

## H1. Narrate allowance (eb86cc0 "Reserve a narrate allowance in the prompt budget")

- `core::BudgetSettings::narrateAllowance` (placed right after `narrateMaxTokens`, so `RoomyBudget()` is `{64, 64, 48, 512, 256, 512, 256}`).
- `LoadSettings`: required `budget.narrate_allowance`, integer in `1..context_size`, included in the "no room for the transcript" sum. Added to `settings.example.json` and the local `settings.json` (512; the local file is not staged).
- `AssembleResolvePrompt`: `transcriptBudget` subtracts the allowance. `contextOverflow` adds the allowance plus the narrate message overhead, measured once per call as `CountPromptTokens({User, ""})`. The data-error text mentions the narrate allowance. Header documents both.
- `ExtendForNarrate` doc: the appended user message must stay within `budget.narrate_allowance`; the turn layer checks this.
- Tests: Settings JSON updated, plus sections (missing `narrate_allowance`, above context size, counts toward transcript room). PromptBuilder budgets updated, plus "The narrate call fits the context when its extra message stays within narrate_allowance" (transcript exactly fills its budget, resolve output of resolve_max_tokens raw tokens, resolution of exactly `narrateAllowance` raw tokens; asserts `CountPromptTokens(narrate) + narrateMaxTokens <= ContextSize()`), plus an exact-boundary test for the overflow flag (tail allowance = tail + 4 + 4 is fine, one less overflows).
- RED (after adding only the field): `./build/tests/bin/Debug/morpheus_tests.exe "[settings],[context]"` ended `test cases: 13 | 7 passed | 6 failed, assertions: 90 | 73 passed | 17 failed` (budget and overflow expectations).
- GREEN: same command, `All tests passed (102 assertions in 13 test cases)`.
- The `<utility>`/`<format>` include sort in `PromptBuilder.cpp` (H3 item 5) landed in this commit, since the file was already being edited.

## H2. UTF-8 model paths (cddbbcb "Pass UTF-8 model paths to llama.cpp")

- `loadModel` keeps a `std::u8string` alive (`path.u8string()`) and passes `reinterpret_cast<const char*>(u8.c_str())` to `llama_model_load_from_file`.
- `EmbeddingModelId()` returns the UTF-8 stem. Error messages use `generic_u8string()`. A small anonymous-namespace helper `Utf8(const std::u8string&)` converts to `std::string` (four callers).
- New `[llama]` test "Model files in non-ASCII paths load, and the embedding model id is UTF-8": hard-links both models into a folder with Japanese names (no gigabyte copies), loads a second backend (CPU, ctx 512), checks the id. It skips if hard links cannot be made.
- RED: `REQUIRE_NOTHROW(make_unique<LlamaCppBackend>(config))` failed with `unknown error` (the ANSI conversion of the path throws).
- GREEN: `All tests passed (2 assertions in 1 test case)`.

## H3. Small fixes and doc drift (9e12d85 "Fix small review items and doc drift")

1. `World.cpp`: stray CR error is now `... stray carriage return on line N`, N 1-based in the original file (counted while normalizing). Test uses `"line one\r\nline two\rline three\n"` and asserts `carriage return`, `rules.md` and `line 2`. RED: old message was `... at byte 17`, failing the `line 2` check. GREEN after the change.
2. `.gitignore`: `/models/`, `/settings.json`.
3. SettingsTests: missing `context_size`, `gpu_layers: "all"`, malformed JSON (`{`, asserts `settings.json`).
4. `LlamaEmbedStateTests.cpp`: the two checks are `CHECK_THROWS_MATCHES(..., InferenceError, ...)`.
5. Includes: `<cstddef>` in StateView.cpp; `<cstddef>`, `<utility>`, `<vector>` in Transcript.cpp; PromptBuilder.cpp sorted (in H1).
6. Docs: Llm.h `Complete` comment, RunStore.h `Turn`/`Turns` comment, StateView.h precondition, CMake `MORPHEUS_BUILD_APP` description, CLAUDE.md core line, architecture spec (mechanics example, interface block, budget bullet with the narrate allowance).
7. Rule compliance: store re-exports removed, `core::TurnRecord`/`core::Summary`/`core::Note` qualified in RunStore.h/.cpp and RunStoreTests.cpp (test file gets `namespace core = morpheus::core;`). LlamaCppBackend.cpp: `TOKEN_PIECE_BUFFER_BYTES` (the error text uses it), `pMemory` everywhere, `BatchGuard::batch` documented rather than initialized (the constructor init makes a default initializer redundant, the brief's second option).

## Final results

- `ctest --preset tests`: 100% passed, 99 tests.
- `ctest --preset asan`: 100% passed, 99 tests.
- `ctest --preset dev` (model env vars set, so `[llama]` ran): 100% passed, 114 tests (also 114 passed with the vars unset, llama tests skipped).
- `[llama]` run: `morpheus_tests.exe "[llama]"` -> `All tests passed (55 assertions in 15 test cases)`.
- Probe: `./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."`
  - `prompt: 445 tokens (engine rules 16, world 274, tail 142; transcript 0 of 12246 budget); fits: yes`
  - run 1: `[prompt 445 tokens: 0 reused, 445 evaluated in 130 ms; 120 generated in 1957 ms]`
  - run 2: `[prompt 445 tokens: 444 reused, 1 evaluated in 73 ms; 125 generated in 1861 ms]`

## Deviations from the brief (mechanical)

- `CHECK_THROWS_MATCHES(expr, InferenceError, ContainsSubstring(...))` does not compile (a string matcher cannot match an exception). Used `MessageMatches(ContainsSubstring(...))`.
- `Utf8()` helper added in LlamaCppBackend.cpp for the u8string-to-std::string conversion.
- Field order: `narrateAllowance` sits after `narrateMaxTokens`, shifting positional initializers (only `RoomyBudget()` used one).

## Concerns

- The narrate prompt also appends the resolve output as an assistant message. Its template overhead (about 4 tokens with the fake backend) is not reserved: only the raw `resolveMaxTokens` is. In practice the tail allowance is rarely full, so slack absorbs it, but a worst case could exceed the context by a few tokens. The turn layer (milestone 3) could add the assistant-message overhead to the check, or the overflow check could count it.
- The narrate message overhead is approximated by `CountPromptTokens({User, ""})`. A chat template that adds a default system block to a prompt with no system message would overestimate it (safe direction).
- The non-ASCII path test needs hard links on the same volume as the temp folder (it skips otherwise) and loads a second copy of both models on CPU (about 2.6 GB of RAM while it runs).
- Working-tree line endings are mixed (some files CRLF, index LF); not touched.

## Fix round 1

Gap: the narrate prompt is the resolve prompt plus an assistant message (resolve output, up to resolveMaxTokens) and a user message (resolution + instruction, up to narrateAllowance). `contextOverflow` reserved the user message's template overhead but not the assistant's, so `Fits()` could be true while the narrate call overflowed by a few tokens.

Changes (`src/context/PromptBuilder.cpp/.h`, `tests/context/PromptBuilderTests.cpp`):
- `AssembleResolvePrompt` measures the assistant overhead once (`CountPromptTokens` of `{Assistant, ""}`) and adds it to the `contextOverflow` sum. Header comment lists both overheads.
- Exact-boundary test: `tailAllowance` is now `tail + 12` (fits) and `tail + 11` (overflows); +4 for the assistant template tokens on the fake backend.
- The narrate-fit test is rewritten to prove the bound. The tail uses its whole allowance (`tailAllowance == tail`), the resolve output is exactly `resolveMaxTokens` raw tokens and the resolution exactly `narrateAllowance` raw tokens. The needed context is measured from the real worst-case narrate prompt (`CountPromptTokens(narrate) + narrateMaxTokens`, on a roomy backend). At exactly that context `Fits()` must be true and `CountPromptTokens(narrate) + narrateMaxTokens <= ContextSize()` holds. At 1..4 tokens less, `contextOverflow` must be set (with `transcriptOverBudget` false) and `Fits()` false.
- Deviation: the brief asked for "transcript exactly fills its budget" and "tail uses its whole allowance" together with `Fits()` exactly true. These cannot all hold: the tail message's 4 template tokens plus the two narrate overheads (8) are in `contextOverflow` but not in `transcriptBudget`, so at the exact `Fits()` boundary the transcript has 12 tokens of budget slack. I kept tail-full and exact `Fits()` boundary (the bound is then tight: equality), and assert the transcript budget is at least the transcript cost.

RED (src change stashed, new tests in place):
- `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[context]"`
- Result: 11 test cases, 9 passed, 2 failed. The new test: 17 assertions, 8 failed (`CHECK( assembly.contextOverflow )` false and `CHECK_FALSE( assembly.Fits() )` true at needed-1..needed-4). The boundary test also failed (`tight` case: contextOverflow false).

GREEN (fix restored):
- `cmake --build --preset tests && ctest --preset tests`: 100% passed, 99 tests.
- `cmake --build --preset dev`: built.
- `./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."`: `prompt: 445 tokens (engine rules 16, world 274, tail 142; transcript 0 of 12246 budget); fits: yes`
