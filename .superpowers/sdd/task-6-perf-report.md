# Task 6 (perf): grammar-constrained generation speed

Status: DONE_WITH_CONCERNS
Commit: d6ed70f Check the grammar only against the sampled token, resampling when rejected

## What was implemented

`LlamaCppBackend::Complete` (src/llama/LlamaCppBackend.cpp):
- The grammar is now its own sampler (`pGrammar`), not the first link of the chain. It is still built before the prompt is
  decoded, so a grammar that does not parse throws ContractError before the KV cache is touched.
- The chain (`pChain`) holds only greedy, or top_k / top_p / temp / dist.
- For each step, a reused candidate buffer is filled from `llama_get_logits_ith(ctx, -1)`, and the chain picks a token.
  With a grammar, only that token is checked: it goes through the grammar sampler as a one-element array. If its logit
  comes back -inf (rejected), the buffer is refilled, the grammar masks the full set, and the chain samples again. The
  final token is accepted into the grammar sampler and into the chain. This mirrors `common_sampler_sample` with
  `grammar_first == false`.
- With greedy decoding the result is identical to the grammar-first chain: argmax over all tokens, when allowed, is the
  argmax over the allowed tokens. The resolve test printed byte-identical output before and after.
- Seeded sampling stays deterministic. A rejected step draws from the dist RNG twice, so the random stream differs from
  the old chain. It is still a pure function of the seed.
- Streaming and UTF-8 handling are unchanged.
- `llama_sampler_sample` is no longer called. Its backend-sampler path never ran here, because no backend samplers are
  configured.
- Two local lambdas, each with two callers: `fillCandidates` and `selectedToken`. Both are guarded by MORPHEUS_REQUIRE:
  missing logits, or no selected token.

The new test is in tests/llama/LlamaCompletionTests.cpp: "LlamaCppBackend keeps output in the grammar when the model's
own choice is rejected" [llama]. It runs in three parts:
1. Precondition: unconstrained greedy on "Repeat this word exactly and nothing else: hello" starts with "hello", so the
   grammar must reject the model's first choice.
2. Grammar `root ::= "yes" | "no"`, greedy: the output is "yes" or "no", and a second run gives the same text.
3. Temperature 1.5 with seeds 1 to 4: every output is "yes" or "no", and the same seed gives the same text.

The test also passed with the old chain (it pins the behavior through the rewrite, as the brief expected).

## Measurement command

All runs used this command, from the repository root in Git Bash, with `<cfg>` set to Debug or RelWithDebInfo:

    export MORPHEUS_TEST_CHAT_MODEL="C:/Users/user/Desktop/MorpheusEngine/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf" && \
    export MORPHEUS_TEST_EMBED_MODEL="C:/Users/user/Desktop/MorpheusEngine/models/nomic-embed-text-v1.5.Q8_0.gguf" && \
    ./build/dev/bin/<cfg>/morpheus_tests.exe "llama.cpp accepts the generated grammars and they constrain real output"

The RelWithDebInfo binaries were built into the same build tree's per-configuration folders, without touching
CMakePresets.json:

    cmake --build --preset dev --config RelWithDebInfo --target morpheus_tests morpheus_probe

This worked as is with the multi-config VS generator. The outputs are in build/dev/bin/RelWithDebInfo. The first build
took about 10 minutes, because it compiles llama.cpp and the Vulkan shaders.

## Results (resolve call, 129 tokens, identical output in every run)

| Build          | Old chain (grammar first) | New (lazy grammar check)        |
|----------------|---------------------------|---------------------------------|
| Debug          | 1.8 tok/s (70497 ms)      | 12.5 / 13.9 / 14.9 tok/s        |
| RelWithDebInfo | 13.1 tok/s (9879 ms)      | 25.2 / 26.7 / 32.6 tok/s        |

Debug improved about 7-8x and RelWithDebInfo about 2-2.5x. The Debug build factor is about 2x on the new code and about
7x on the old code.

WARN lines, verbatim. The output is the same each time, so it is truncated after the first line:

    Debug, before:          resolve: 129 tokens in 70497 ms (1.8 tok/s); output: {"intent":"action", ...
    Debug, after:           resolve: 129 tokens in 10305 ms (12.5 tok/s); output: {"intent":"action", ...
    Debug, after (final):   resolve: 129 tokens in 9267 ms (13.9 tok/s); output: {"intent":"action", ...
    RelWithDebInfo, old:    resolve: 129 tokens in 9879 ms (13.1 tok/s); output: {"intent":"action", ...
    RelWithDebInfo, after:  resolve: 129 tokens in 4836 ms (26.7 tok/s); output: {"intent":"action", ...
    RelWithDebInfo, final:  resolve: 129 tokens in 3959 ms (32.6 tok/s); output: {"intent":"action", ...

Full output, from every run:

    {"intent":"action","checks":[{"stat":"agility","difficulty":"medium","stakes":"climbing onto the crawler's roof",
    "on_success":[{"op":"add","path":"player.conditions","value":"on_roof","reason":"Successfully climbed onto the
    crawler's roof to scan the horizon"}],"on_failure":[{"op":"add","path":"player.conditions","value":"failed_climb",
    "reason":"Failed to climb due to low agility and instability"}]}],"changes":[{"op":"add","path":"player.conditions",
    "value":"on_roof","reason":"Player successfully climbed onto the crawler's roof to scan the horizon"}]}

## Where the remaining time goes (temporary instrumentation, reverted, not committed)

I timed the chain step and the grammar work per call, and counted the rejections. The chain step includes the GPU sync
inside `llama_get_logits_ith`.

- RelWithDebInfo, grammar on: 18 of 129 tokens rejected. Chain 2770 ms, grammar 1035 ms, total 4242 ms.
  - One full-vocabulary grammar pass costs about 55 ms in RelWithDebInfo and about 290 ms in Debug. The Debug figure
    comes from the 4-token probe calls: 1173 ms for 4 rejections.
  - Rejected pieces were mostly tokenization boundaries: `{`, ` "`, `":`, `":"`, `,"`, `"},{"`, `":{"`, `":["`. A few were
    content words the grammar does not allow: `type`, `success`, `effect`, `.location`, `intent`.
- The same prompt with no grammar (the test's spec.grammar cleared temporarily) runs at about 48-57 tok/s in
  RelWithDebInfo and about 71 tok/s in Debug.
  - The brief's "21.7 tok/s unconstrained" matches the Debug probe, which uses top_k sampling. top_k over 151k tokens is
    slow under Debug iterator checks.
  - The greedy unconstrained path on the same prompt is faster than that.
- So after the fix, the gap to unconstrained is almost entirely the full grammar passes on rejected steps, about 14% of
  steps here. Debug makes each pass about 5x slower.

## Tests

- `./build/dev/bin/Debug/morpheus_tests.exe "[llama]"`: All tests passed (81 assertions in 20 test cases).
- `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"`: All tests passed (81 assertions in 20 test cases).
- New test alone (Debug): All tests passed (11 assertions in 1 test case). It also passed against the old chain before
  the rewrite.
- `ctest --preset dev`, with the env vars: 100% tests passed out of 143.

## Probe

Command: `./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."`

Debug, final code:

    prompt: 723 tokens (engine rules 16, world 552, tail 142; transcript 0 of 11968 budget); fits: yes
    [prompt 723 tokens: 0 reused, 723 evaluated in 330 ms; 115 generated in 3552 ms]
    [prompt 723 tokens: 722 reused, 1 evaluated in 92 ms; 119 generated in 1612 ms]

RelWithDebInfo, same command:

    [prompt 723 tokens: 0 reused, 723 evaluated in 202 ms; 115 generated in 1351 ms]
    [prompt 723 tokens: 722 reused, 1 evaluated in 6 ms; 119 generated in 1284 ms]

Run 2 reuses 722 of 723 prompt tokens.

## Files changed

- src/llama/LlamaCppBackend.cpp
- tests/llama/LlamaCompletionTests.cpp

## Deviations

- The brief suggested a prompt asking for the word "hello". The first wording, "Reply with exactly one word: hello.",
  made the model answer "hi". I changed it to "Repeat this word exactly and nothing else: hello" and made the
  precondition a REQUIRE that the unconstrained output starts with "hello", so the test proves the rejection path is
  taken.
- Measurement used temporary edits: a counter and timers in Complete, and a cleared grammar in LlamaGrammarTests.cpp.
  Both were reverted before the commit, and the committed diff contains neither.

## Concerns

- Constrained decoding is still slower than unconstrained: about 30 vs about 50 tok/s in RelWithDebInfo, and about 14
  vs about 70 tok/s greedy in Debug. The cost is the full-vocabulary grammar pass on rejected tokens, about 55 ms each in
  RelWithDebInfo.
- A further exact optimization is possible, but not implemented (beyond the brief). On rejection, check candidates in
  descending logit order and stop at the first the grammar accepts. For greedy this is exactly the argmax over allowed
  tokens. For sampling it would need a top-k cut first.
- Another lever: steer the model toward the grammar's tokenization (e.g. the prompt's JSON example matching the
  grammar's spacing), since most rejections were `{` / `":` boundary tokens.
- The Debug build also costs about 2x on the new code. Whether the dev preset should build llama.cpp optimized is the
  controller's call; the presets were not changed.
- Seeded (temperature > 0) sampling now uses rejection sampling, like llama.cpp's common. A seed gives the same text
  every run, but not the same text as the old grammar-first chain gave. Greedy output is unchanged.
- build/dev now also contains a RelWithDebInfo build of llama.cpp, morpheus_tests and morpheus_probe (disk use only).
