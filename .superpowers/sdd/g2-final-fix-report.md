# G2 final fix round: report

HEAD at start d2793b5. Commits:
- e32e765 Item 1 (+ item 3): close text refusal at load, live test fixes
- a2331ad Item 2: HasThinkingSwitch, PUBLIC core linkage, min_p test, <cstddef>
- db49382 Item 4: re-wraps, architecture spec min_p

## Item 1
- RED: `morpheus_tests.exe "LlamaServerBackend refuses a thinking format*"` -> 2 of 3 assertions failed ("no exception was thrown").
- Both backends now throw InferenceError "the thinking format's close text uses N tokens; it must be under 64 (the smallest
  thinking budget)" when the tokenized close has MIN_THINKING_TOKENS tokens or more; the per-call MORPHEUS_REQUIRE stays.
  Constructor doc comments in both headers updated.
- GREEN: `"[thinking]"` -> all passed. The fake-server test: 256 chars (64 tokens) throws, 252 chars (63) loads.
- `[llama]` test "A thinking format whose close text fills the smallest budget is refused at load" (100 x "word " plus
  "</think>\n\n"; skips without models).

## Item 2
- `core::HasThinkingSwitch(std::string_view)` in Thinking.h/.cpp, used by ResolveThinking and LlamaServerBackend; unit test.
- morpheus_core is PUBLIC in src/llama and src/llamaserver CMakeLists. `cmake --preset tests` configures; consumer check
  (`cmake -S cmake/consumer-check -B build/consumer-check`, build, run) -> "consumer_check: ok".
- CallTagTests includes <cstddef>. min_p bad-value loop uses `nlohmann::json(bad).dump()` and adds the string "0.1".

## Item 3
- LlamaTestModels.h: new `RequireThinkingModel()` (SKIP outside any CHECK); MakeThinkingBackend calls it and loads the model.
- The end-marker test and the new close test call `RequireThinkingModel()` first, then CHECK_THROWS_WITH.
- The second (Format qwen) pass and its comment are removed from the thinking test; MakeThinkingBackend stays (used by
  SharedThinkingBackend and the two refusal tests).

## Item 4
- Re-wrapped: SettingsEdit.cpp samplingJson, LlamaServerCompleteTests.cpp (two long lines of G2), Settings.h thinking field,
  LlamaServerBackend.h (templateWritesOpen comment, constructor comment), LlamaServerBackend.cpp comment, LlamaCppBackend.cpp
  ResolveThinking call (local `chatTemplate`).
- Architecture spec llama-server bullet: "samplers top_k, top_p, min_p, temperature".

## Final
- `cmake --build --preset tests --clean-first`: clean (no warnings/errors printed).
- `ctest --preset tests`: 100% tests passed out of 707.
- Dev build and `[llama]` skip run: see the end of this file.
- Byte checks on every changed file: ASCII count 0 and control-byte count 0 for all (files printed nothing nonzero).

## Deviations
- Items 1 and 3 share one commit (e32e765): the same two test files hold both changes, and the live test faults were the
  reason the item 1 live test is written as it is.
- Items 2 and 4 also touch the same backend files (comments); the small re-wraps in LlamaServerBackend.cpp/.h and
  LlamaCppBackend.cpp went into the item 2 commit.

## Review minors left
Minor 2 (README budget bullet), 3 and 4 (none/custom on a template with the switch: the owner's follow-up), 7 (call tag
constants), 8 (misleading error for a non-string non-object thinking_format), 9 (covered by item 3), 10 (editor keeps
thinking_format test), 11 (empty afterEnd guard), the retag of the [sampling] test.

## Dev build and [llama] run
- `cmake --build build/dev --config RelWithDebInfo` (foreground, completed): no errors; the only filtered line is cmake's
  ccache warning. All targets (morpheus, morpheus_play, morpheus_probe, morpheus_server, morpheus_tests) linked.
- `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"` with MORPHEUS_TEST_* unset: test cases: 39 | 39 skipped,
  assertions: 0 (every case skipped, none failed, including the two refusal tests).

## Live-test fix

MakeThinkingBackend(thinking, gpuLayers = -1, contextSize = 8192) in tests/llama/LlamaTestModels.h (comment updated); the two refusal tests in tests/llama/LlamaThinkingTests.cpp pass 0 and 512, so the CPU load does not compete with the shared backend for GPU memory. The only other caller (SharedThinkingBackend) keeps the defaults. Environment check stays before the CHECKs.
Verification: dev build of morpheus_tests OK; `[llama]` with no MORPHEUS_TEST_* set: 39 test cases, 39 skipped, 0 failed; tests preset builds. ASCII and control-byte greps: 0 and 0 on both files. Not run against the real models.
