# G2 final fix round: requirements

The final whole-branch review of sub-project G2 (generation controls) is `.superpowers/sdd/g2-final-review.md`; read it
for the detail. The controller decided the items below (2026-11-16) and adds two faults of the live tests found when it
ran them on the real models. Make each numbered item its own commit, tests first where there is behaviour, in this
order. HEAD at the start: d2793b5.

## 1. A close text too long for the smallest thinking budget is refused at load (Important)

Both backends check `spec.thinkingMaxTokens > close.size()` as a contract (LlamaCppBackend.cpp ~249,
LlamaServerBackend.cpp ~436). A custom `close` of `core::MIN_THINKING_TOKENS` (64) tokens or more passes the settings and
the backend load, then fails every thinking call with a ContractError: a data fault found late with the wrong error.

- Both backends, when they load a resolved format: if its `close` tokenizes to `core::MIN_THINKING_TOKENS` tokens or
  more, throw `InferenceError` "the thinking format's close text uses N tokens; it must be under M (the smallest thinking
  budget)" (N the count, M `core::MIN_THINKING_TOKENS`). The per-call `MORPHEUS_REQUIRE` stays (an engine bug then).
- Test against the fake llama-server (`tests/llamaserver/LlamaServerBackendTests.cpp`): a custom format whose `close` is
  long enough to tokenize to 64 or more tokens with the fake tokenizer (4 characters a token: 300 characters ending
  with the format's `end` and `after_end`) makes the constructor throw that InferenceError; one just under 64 loads.
- A `[llama]` test (skips without the models, written as item 3 says) does the same in-process.

## 2. Small code fixes the review lists

- The `enable_thinking` check is written twice (src/core/Thinking.cpp and src/llamaserver/LlamaServerBackend.cpp ~372).
  Add `bool core::HasThinkingSwitch(std::string_view chatTemplate)` to `core/Thinking.h` (the template has Qwen's
  `enable_thinking` switch) and use it in both places, with a unit test in `tests/core/ThinkingTests.cpp`.
- `src/llama/CMakeLists.txt` and `src/llamaserver/CMakeLists.txt`: their public headers now expose core types
  (`core::ThinkingSetting` in the configs), so link `morpheus_core` PUBLIC (check `cmake --preset tests` and the
  consumer check still configure; see `cmake/consumer-check/`).
- `tests/turn/CallTagTests.cpp`: include `<cstddef>`.
- `tests/core/SettingsTests.cpp`, the min_p bad-value loop: expect the engine's exact rendering (`nlohmann::json(bad).dump()`,
  e.g. "not 1.0"), and add a string value (`"0.1"`) that fails with the same message shape.

## 3. The live tests' faults (found by the controller on the real models)

- `tests/llama/LlamaThinkingTests.cpp`, "A thinking format whose end marker is not a single token is refused at load":
  without the models it fails instead of skipping, because `MakeThinkingBackend` calls `SKIP` inside the
  `CHECK_THROWS_WITH` expression (the check catches the skip). Check the environment first, outside any CHECK (skip when
  either variable is empty), then CHECK the throw. Write item 1's `[llama]` test the same way.
- The same file, "A thinking model thinks, then writes a grammar-constrained answer": its second pass loads a second copy
  of the thinking model (`MakeThinkingBackend` with the qwen Format) while the shared one stays loaded. With the
  project's 9B thinking model on a 10 GB GPU that load fails ("unable to allocate Vulkan0 buffer"). Remove that second
  pass: in-process, Format qwen on a Qwen template resolves to the same markers as Auto, which the core unit tests pin,
  and the llama-server live test keeps its qwen pass (it loads nothing). Remove the comment about the second copy, and
  `MakeThinkingBackend` if nothing else uses it.

## 4. Formatting and docs

- Re-wrap to about 120 columns: src/app/model/SettingsEdit.cpp (`samplingJson`), the comment in
  tests/llamaserver/LlamaServerCompleteTests.cpp the review names, src/core/Settings.h (~232),
  src/llamaserver/LlamaServerBackend.h (~249 and ~279).
- The architecture spec's llama-server bullet lists the samplers without min_p: add it (top-k, top-p, min-p,
  temperature).

## Finally

- `cmake --build --preset tests --clean-first`, `ctest --preset tests`, then `cmake --build build/dev --config RelWithDebInfo`
  (every dev target): all clean. Foreground only, one at a time, never in the background.
- Byte checks (ASCII, control bytes) on every changed file.
- Report per item, with the commands and output, and the review minors you left.
