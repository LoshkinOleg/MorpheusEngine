# Task 6 report: embeddings and KV-cache save/restore tests

Status: DONE_WITH_CONCERNS (minor, see below)
Commit: 33544f1 "Test embeddings and KV-cache save/restore"

## Implemented
- `tests/llama/LlamaEmbedStateTests.cpp` (new): the brief's three tests verbatim, plus:
  - embedding vectors have the same non-zero size for four texts of different lengths;
  - truncated blob (first 4 bytes) -> `ContractError`, then `Complete` works with `tokensReused + tokensEvaluated == promptTokens`;
  - blob with leading uint64 count = `UINT64_MAX` -> `ContractError` (no crash, no `length_error`), then the same Complete check;
  - extra (not requested): a blob with the count decremented by one (token list and KV data misaligned) -> `InferenceError`, then the same Complete check.
- `src/llama/LlamaCppBackend.cpp` `RestoreState`: both post-clear failure paths now call `llama_memory_clear(memory, true)` before throwing, with `_cachedTokens` left empty. The pos_max mismatch is now an `InferenceError` ("restored KV state does not match its token list") instead of a `ContractError`.

## TDD evidence
- RED: none obtainable for the new tests. The backend already rejected truncated and huge-count blobs with `ContractError` at the up-front checks (those run before any cache change), and the Task 5 code already passed the brief's three tests. The RestoreState change was made first because it was a reviewed requirement with no observable failure from these inputs.
- GREEN: `export MORPHEUS_TEST_CHAT_MODEL=... MORPHEUS_TEST_EMBED_MODEL=...; ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"` -> `All tests passed (49 assertions in 13 test cases)`. llama.cpp stderr noise (grammar parse error from an existing test, "wrong sequence state magic" from the misaligned-blob test) is expected.
- `ctest --preset tests` -> 100% tests passed out of 88.
- No backend bug was exposed by the brief's tests (nothing to root-cause).

## Files changed
- `tests/llama/LlamaEmbedStateTests.cpp` (new)
- `src/llama/LlamaCppBackend.cpp`

## Deviations
- Needed `cmake --preset dev` re-run for the new test file to be globbed in.
- Added `core/Errors.h`, `<cstdint>`, `<cstring>`, `<limits>` includes to the test file.

## Concerns
- The post-restore pos_max mismatch branch is not covered by a test: the misaligned-blob test is rejected earlier by `llama_state_seq_set_data` ("wrong sequence state magic"), so it covers the set_data failure path. Reaching the pos_max branch needs a valid llama.cpp state blob paired with a wrong count, which I could not construct without a second valid state of different length; the clear-then-throw code is symmetric with the covered path.

## Fix round 1

Changes: new TEST_CASE for the pos_max mismatch branch (erase one int32 token after the count, decrement the count; set_data succeeds, InferenceError "restored KV state does not match its token list"); renamed the set_data-failure test; added CheckCacheWasCleared (tokensReused == 0, tokensEvaluated == promptTokens) to both post-clear failure tests only; reworded the RestoreState comment (in-memory GPU hand-off blobs: malformed header = ContractError, llama.cpp-rejected or token-list-inconsistent state = InferenceError); added `<span>` (and `<cstddef>` for ptrdiff_t); added the vector-size test comment.

Commands:
    cmake --build --preset dev        -> built clean
    export MORPHEUS_TEST_CHAT_MODEL=... MORPHEUS_TEST_EMBED_MODEL=...
    ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"
    -> All tests passed (53 assertions in 14 test cases)
       (log noise: "state_seq_set_data: error loading state: wrong sequence state magic" from the set_data-failure test, expected)

Commit: "Cover the restore token-count check and assert cleared caches". The pos_max test reached the mismatch branch (set_data accepted the blob), no weakening needed.
Deviation: added `#include <cstddef>` for std::ptrdiff_t.

## Fix round 2

Changes (tests/llama/LlamaEmbedStateTests.cpp only):
- set_data failure test: `CHECK_THROWS_AS(..., InferenceError)` -> `CHECK_THROWS_WITH(..., Catch::Matchers::ContainsSubstring("restoring the KV cache state failed"))`.
- token-count mismatch test: `CHECK_THROWS_AS(..., InferenceError)` -> `CHECK_THROWS_WITH(..., Catch::Matchers::ContainsSubstring("token list"))`.
- Added `#include <catch2/matchers/catch_matchers_string.hpp>`.
- Reverted the accidental formatting change: `{"hi", "a considerably longer ...`.
- Vector-size test comment now: "Guards against the embedding dimension varying with input length (and with the query prefix)."

Commands:
    cmake --build --preset dev
    -> built clean (morpheus_tests.exe linked; no compile errors)
    export MORPHEUS_TEST_CHAT_MODEL=C:/Users/user/Desktop/MorpheusEngine/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf MORPHEUS_TEST_EMBED_MODEL=C:/Users/user/Desktop/MorpheusEngine/models/nomic-embed-text-v1.5.Q8_0.gguf
    ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"
    -> All tests passed (53 assertions in 14 test cases)
       (log noise, expected: "error parsing grammar" from an existing test; "state_seq_set_data: error loading state: wrong sequence state magic" from the set_data-failure test)

Concern: CHECK_THROWS_WITH does not assert the exception type, so these two checks no longer prove InferenceError specifically. Catch2's CHECK_THROWS_MATCHES with Catch::Matchers::MessageMatches would check both; not changed here because the brief asked for CHECK_THROWS_WITH.
