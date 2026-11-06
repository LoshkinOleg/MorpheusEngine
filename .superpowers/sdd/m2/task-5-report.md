# Task 5 report: LlamaCppBackend

Status: DONE_WITH_CONCERNS (minor; see Concerns)

Commit: b3a7851 Add the in-process llama.cpp backend

## What was implemented

All code is verbatim from the brief. It compiled under /W4 /WX on the first try against llama.cpp 960d628f, so no signature fixes were needed.
- `src/llama/LlamaCppBackend.h/.cpp` (`morpheus::llama`):
  - `LlamaConfig` and `LlamaCppBackend` (implements `inference::LlmBackend`).
  - Loads the chat and embedding models and creates their contexts.
  - Renders the ChatML prompt through `llama_chat_apply_template`.
  - Reuses the longest common token prefix of the KV cache.
  - Generation is chunked, grammar-constrained, and streamed in UTF-8-safe pieces. It can be cancelled.
  - Pooled, L2-normalized embeddings with query/document prefixes.
  - `SaveState`/`RestoreState` blob: token count, tokens, then the llama.cpp sequence state.
- `src/llama/CMakeLists.txt`: the `morpheus_llama` static library (PUBLIC `morpheus_inference`, PRIVATE `llama`), with `morpheus_target_defaults`.
- Root `CMakeLists.txt`: `add_subdirectory(src/llama)` inside `if(MORPHEUS_BUILD_LLAMA)`.
- `tests/CMakeLists.txt`: links `morpheus_llama` when `MORPHEUS_BUILD_LLAMA` is on.
- `tests/llama/LlamaTestModels.h`: a shared lazily-loaded backend that skips without the env vars.
- `tests/llama/LlamaCompletionTests.cpp`: 6 `[llama]` test cases.

## TDD evidence

RED: the backend sources were held aside, and only the tests and fixture were in the tree.
```
$ cmake --build --preset dev --target morpheus_tests
...
  LlamaCompletionTests.cpp
C:\Users\user\Desktop\MorpheusEngine\tests\llama\LlamaTestModels.h(9,1): error C1083: Cannot open include file: 'llama/LlamaCppBackend.h': No such file or directory
```

GREEN:
```
$ cmake --build --preset dev            -> morpheus_llama.lib, morpheus_tests.exe built, no warnings/errors
$ ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"       (no env vars)
test cases: 6 | 6 skipped
$ export MORPHEUS_TEST_CHAT_MODEL=.../Qwen3-4B-Instruct-2507-Q4_K_M.gguf
$ export MORPHEUS_TEST_EMBED_MODEL=.../nomic-embed-text-v1.5.Q8_0.gguf
$ ./build/dev/bin/Debug/morpheus_tests.exe "[llama]" --durations yes
9.299 s: LlamaCppBackend rejects requests that cannot work   (includes the one-time model load)
0.954 s: LlamaCppBackend follows a GBNF grammar
0.003 s: LlamaCppBackend returns early when cancelled
0.003 s: LlamaCppBackend counts tokens
1.863 s: LlamaCppBackend generates and streams text
0.758 s: LlamaCppBackend reuses the cached prompt prefix
All tests passed (18 assertions in 6 test cases)
```
I re-ran with `--order decl` and `--order rand`, and all tests passed both times.

Full suites:
- `ctest --preset tests`: 100% passed out of 88 (that preset has no llama.cpp).
- `ctest --preset dev` with both env vars exported: 100% passed out of 94, including the 6 `[llama]` tests.

No `[llama]` test failed, so I did no debugging and adjusted no test. The prefix-reuse counts hold exactly: `tokensReused == promptTokens - 1` and `tokensEvaluated == 1`.

## Files changed

- CMakeLists.txt
- tests/CMakeLists.txt
- src/llama/CMakeLists.txt (new)
- src/llama/LlamaCppBackend.h (new)
- src/llama/LlamaCppBackend.cpp (new)
- tests/llama/LlamaTestModels.h (new)
- tests/llama/LlamaCompletionTests.cpp (new)

## Deviations from the brief

- The RED build targeted `morpheus_tests` only (`--target morpheus_tests`) instead of the whole `dev` build. The failure is the expected C1083.
- No code deviations.

## Concerns

- `Embed`, `SaveState` and `RestoreState` are implemented but none of this task's tests exercises them; the brief adds only completion tests. They are untested until a later task covers them.
- The `[llama]` run prints llama.cpp warnings to stderr. The brief's `LogFilter` passes WARN and ERROR through by design. They are:
  - Qwen GGUF tokenizer notes: the `</s>` control-token override and `special_eos_id`.
  - The `n_ctx_seq < n_ctx_train` notice.
  - The expected grammar parse error from the bad-grammar test.

  None of them is a failure.
- I could not confirm Vulkan GPU offload from the output, because INFO logs are filtered. The timings (about 1.9 s for 24 tokens in a Debug build) suggest the GPU is in use.
- The new files were written with LF endings. Git warned that it will convert them to CRLF under the repo's autocrlf setting. This is harmless.

## Fix round 1

Changes in `src/llama/LlamaCppBackend.cpp` (header untouched):
1. Embed sizes the pooled vector with `llama_model_n_embd_out`; the constructor also rejects `LLAMA_POOLING_TYPE_RANK` (InferenceError, "is a reranker, not a sentence-embedding model").
2. The sampler chain (grammar check included) is now built right after the context-size check, before the KV-cache prefix handling and prompt decode. An invalid GBNF throws ContractError before any decode.
3. Context check is `spec.maxTokens > ContextSize() - completion.promptTokens`, same message.
4. Early cancel during prompt evaluation sets `completion.promptMs` before returning.
5. RestoreState: `MORPHEUS_REQUIRE(count <= (state.size() - sizeof(count)) / sizeof(int32_t))` before computing `tokenBytes`/allocating; after a successful `llama_state_seq_set_data`, `MORPHEUS_REQUIRE(llama_memory_seq_pos_max(mem, 0) + 1 == count)`. llama.h documents pos_max as -1 for an empty sequence, so count 0 satisfies the check unchanged.

Commands:
- `cmake --build --preset dev` (clean build)
- `MORPHEUS_TEST_CHAT_MODEL=... MORPHEUS_TEST_EMBED_MODEL=... ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"` -> `All tests passed (18 assertions in 6 test cases)`
- `ctest --preset tests` -> `100% tests passed out of 88`
