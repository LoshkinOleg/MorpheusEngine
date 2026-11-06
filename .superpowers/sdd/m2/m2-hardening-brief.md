# M2 hardening round: requirements

Context: milestone 2 is done and has passed its final whole-branch review. The fixes below land before milestone 3 (the turn layer) builds on this code. No runs and no committed settings.json exist, so changing the settings format is free.

**Rules for every section:**
- Follow `CLAUDE.md`, `.superpowers/sdd/global-constraints.md` and `.superpowers/sdd/implementer-instructions.md`.
- Use TDD where behavior changes: write the test, show it failing, implement, show it passing.
- Make one commit per numbered section.
- Never stage `dotnet/`, `models/` or `settings.json`.
- After H1, the local `settings.json` at the repo root must load. Add the new key to it too: edit it, but do not stage it.

---

## H1. Budget the narrate call's extra message (Important)

`ExtendForNarrate` appends a user message (the resolution record plus the narrate instruction). No budget reserves it, so a prompt with `Assembly::Fits() == true` can overflow the context during the narrate call.

1. **Settings.** Add `narrate_allowance` to settings `budget`:
   - Add it to `core::BudgetSettings` as `int narrateAllowance = 0;` with the comment "Resolution record + narrate instruction appended for the narrate call."
   - In `LoadSettings`, it is required, an integer in `1..context_size`, and included in the "leaves no room for the transcript" sum.
   - Add `"narrate_allowance": 512` to `settings.example.json` and to the local `settings.json`.
   - Update the existing Settings tests' JSON and add a test that it is required.
2. **`AssembleResolvePrompt`.**
   - Subtract `budget.narrateAllowance` in `transcriptBudget`.
   - Add it in the `contextOverflow` check, along with the narrate message's template overhead. Approximate that overhead as the per-message overhead that the backend reports: the cost of a one-message prompt `{User, ""}`, `CountPromptTokens`. Compute it once.
   - Document both in the header.
   - Update the existing PromptBuilder tests' expected budgets (`RoomyBudget()` gets a `narrateAllowance` value) and add a test: a transcript that exactly fills its budget, then `ExtendForNarrate` with a resolution string whose raw tokens equal `narrateAllowance`. The narrate prompt's `CountPromptTokens` plus `narrateMaxTokens` must be <= the context size.
3. **`ExtendForNarrate`.** Doc comment: "the appended user message must stay within budget.narrate_allowance; the turn layer checks this".

## H2. UTF-8 model paths into llama.cpp

`src/llama/LlamaCppBackend.cpp` passes `path.string()` (the ANSI code page) to `llama_model_load_from_file`, but ggml opens files as UTF-8 on Windows.
- Pass the UTF-8 bytes instead: keep a `std::u8string` alive and pass `reinterpret_cast<const char*>(u8.c_str())`.
- Apply the same fix to `EmbeddingModelId()`'s `stem().string()`: return the UTF-8 stem as `std::string`.
- Use `generic_u8string()`-based conversion in the backend's error messages.

## H3. Small fixes and doc drift

1. **`src/core/World.cpp` stray-CR error.** Report the 1-based line number in the original file instead of "byte N" of the normalized text. In `tests/core/WorldTests.cpp`, the lone-CR test also asserts the message contains `rules.md`.
2. **`.gitignore`.** Anchor the entries: `/models/` and `/settings.json`, replacing the unanchored lines.
3. **`tests/core/SettingsTests.cpp`.** Add failure sections:
   - a missing required key (`context_size` removed);
   - a wrong type (`gpu_layers: "all"`);
   - malformed JSON (`{`).

   Each asserts the message names the key or the file.
4. **`tests/llama/LlamaEmbedStateTests.cpp`.** The two `CHECK_THROWS_WITH(..., ContainsSubstring(...))` become `CHECK_THROWS_MATCHES(expr, morpheus::inference::InferenceError, ContainsSubstring(...))`, so the exception type is pinned again.
5. **Includes.**
   - `src/context/StateView.cpp` and `Transcript.cpp`: add `<cstddef>`, `<utility>`, `<vector>` where used.
   - `src/context/PromptBuilder.cpp`: sort the standard includes alphabetically.
6. **Doc drift:**
   - `src/inference/Llm.h`, the `Complete` comment: add "Throws ContractError for maxTokens <= 0 or a grammar that does not parse."
   - `src/store/RunStore.h`, the `Turn`/`Turns` comments: one sentence: "Throws StoreError if a stored column is not valid JSON or has the wrong shape, or a note has no matching turn row."
   - `src/context/StateView.h`: state its precondition that `state` must be validated state (schema.ValidateState / ApplyChanges output). Keep the ContractError mention for the shape checks it does make.
   - Root `CMakeLists.txt`: the `MORPHEUS_BUILD_APP` option description must say "(needs MORPHEUS_BUILD_LLAMA; fetches SDL3 and ImGui)".
   - `CLAUDE.md` layout line for src/core: "its only I/O is reading world folders and settings.json".
   - Architecture spec:
     - The mechanics-line example becomes `[agility vs hard: 14/16 failure; player.location = "deck"]`.
     - The interface block also lists `count_prompt_tokens(prompt)`, `context_size()`, `embedding_model_id()`, and `embed(texts, purpose: query|document)`.
     - The budget bullet mentions the narrate allowance.
7. **CLAUDE.md rule compliance in the code:**
   - **The re-exports.** Remove the `using core::Note; using core::Summary; using core::TurnRecord;` re-exports from `src/store/RunStore.h`. Qualify every use (store code and tests) as `core::TurnRecord` and so on. This is the legacy-path rule.
   - **`LlamaCppBackend.cpp`:**
     - The token-piece buffer size 256 becomes a constant `TOKEN_PIECE_BUFFER_BYTES`.
     - `BatchGuard::batch` gets an initializer at its declaration. If the ctor-init makes that redundant, keep the ctor init and add a comment instead.
     - The local `llama_memory_t memory` becomes `pMemory` (a pointer typedef), everywhere in that file.

## Report

Write `.superpowers/sdd/m2-hardening-report.md` with one section per H number: what changed, RED/GREEN where relevant, and the commit SHA.

Finish with full results:
- `ctest --preset tests`;
- `ctest --preset asan`;
- `ctest --preset dev`;
- the `[llama]` run with the model env vars;
- a probe run: `./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."`. Paste its two token lines.
