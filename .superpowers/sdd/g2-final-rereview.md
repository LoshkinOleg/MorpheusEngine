# G2 final re-review: the fix round (d2793b5..db49382)

Read-only re-review of e32e765, a2331ad and db49382 against `.superpowers/sdd/g2-final-review.md`, the decisions in
`.superpowers/sdd/g2-final-fix-brief.md` and the report `.superpowers/sdd/g2-final-fix-report.md`. No builds or tests
run (the controller runs asan and the live suites).

## The findings, one by one

| Finding | Status |
|---|---|
| Important 1: a close text too long for the smallest budget | Fixed in both backends. See below. |
| Minor 1: the architecture spec's sampler list | Fixed: "top_k, top_p, min_p, temperature". The settings bullet still does not mention min_p or thinking_format; the brief did not ask for that. |
| Minor 5: the enable_thinking check written twice | Fixed: `core::HasThinkingSwitch` is used by `ResolveThinking` and `LlamaServerBackend`, with a unit test (with the switch, without it, and an empty template). |
| Minor 6: the backends' link interface | Fixed: `morpheus_core` is PUBLIC in both src/llama and src/llamaserver. The report says the consumer check prints ok. |
| T1: the min_p bad-value test | Fixed: it expects `nlohmann::json(bad).dump()` and adds the string `"0.1"`, which reaches the `!is_number()` branch. |
| T2: `<cstddef>` in CallTagTests | Fixed. |
| Formatting (SettingsEdit, Settings.h, LlamaServerBackend.h and .cpp, LlamaServerCompleteTests) | Fixed. |
| Live fault: SKIP inside CHECK_THROWS | Fixed: `RequireThinkingModel()` runs before the CHECK in both refusal tests. |
| Live fault: the second model copy in the thinking test | Fixed in that test: the Format pass is gone, and the llama-server live test keeps its qwen pass. Not fixed in the two refusal tests: see Important R1. |
| Minors 2, 3, 4, 7, 8, 10, 11 and the [sampling] retag | Left, as the brief decided. These can wait. Minors 3 and 4 are the owner's follow-up. |

### Important 1, checked

- Both backends check the tokenized `close` against `core::MIN_THINKING_TOKENS` at load and throw InferenceError,
  with the brief's exact text. The per-call `MORPHEUS_REQUIRE` stays as an engine contract.
- In-process, the check runs after the single-token `end` check and before the embedding model loads.
- Over llama-server, it runs before `end` and `afterEnd` are tokenized, so a refused format costs no extra requests.
- The constructor comments in both headers name the new refusal.
- The fake-server test pins the boundary (256 characters are 64 tokens and are refused; 252 are 63 and load) and the
  exact message.
- Every non-zero budget is at least 64 tokens, so the invariant `close < MIN_THINKING_TOKENS <= budget` now holds
  for any data that loads.
- Including `core/Settings.h` in both backend .cpp files, for the constant, is fine: core is a declared dependency.

## Remaining issues

### Critical

None.

### Important

**R1. The two in-process refusal tests still load a second full copy of the thinking model while the shared one
stays loaded.** tests/llama/LlamaThinkingTests.cpp:131-152, tests/llama/LlamaTestModels.h:52.

- What happens: "end marker is not a single token" and "close text fills the smallest budget" both call
  `MakeThinkingBackend`. That loads the chat model with `gpuLayers = -1` and creates an 8192-token context, and only
  then checks the format (LlamaCppBackend.cpp: the chat model and context come first). Catch2 runs the cases in
  file order, so the static `SharedThinkingBackend` from the four earlier cases is still alive.
- Why it matters: this is the same setup that ran out of memory in the controller's live run ("unable to allocate
  Vulkan0 buffer", the 9B model on a 10 GB GPU). In that run the constructor throws "cannot load model '...'" instead
  of the refusal, and `CHECK_THROWS_WITH` fails both cases. The fast suite and a run without models cannot show it:
  every [llama] case skips there, which is what the report shows (39 skipped).
- How to fix (both refusals happen at load, before anything is generated): give `MakeThinkingBackend` a GPU-layer
  argument, and have the refusal tests pass 0 layers and a small context such as 512. The second copy then loads on
  the CPU, from the memory-mapped file. Alternatively, move the two refusal cases into their own `[llama]` TEST_CASEs
  in a separate file that never touches `SharedThinkingBackend`. That only helps if they run in a separate process,
  so the CPU load is the robust fix.
- If the controller's live run on the 9B model passes both cases, downgrade R1 to Minor (it would then depend on the
  GPU's free memory). Keep the 0-layer load anyway.

### Minor

- The in-process close test assumes 100 times "word " is at least 64 tokens. That is safe for any BPE vocabulary
  (each " word" is at least one token).
- The architecture spec's settings bullet still omits min_p and thinking_format (left over from Minor 1; not asked).

## Assessment

**Ready to merge?** With fixes

**Reasoning:** Every decided item is implemented correctly, and Important 1 is closed in both backends with
boundary tests. R1 remains: the two in-process refusal tests still load a second full GPU copy of the thinking
model while the shared one is held. That is the out-of-memory pattern the controller hit, and it will likely fail
both cases on the 9B model. Loading those copies on the CPU closes it, unless the controller's live run already
shows them passing.
