### Task 11: Documentation and full verification

The architecture spec, the README, `CLAUDE.md` and the consumer check describe what Tasks 1-10 built; then every preset is built and tested once more.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `cmake/consumer-check/CMakeLists.txt`

**Interfaces:**
- Consumes: the names of Tasks 1-10.
- Produces: documentation only.

- [ ] **Step 1: Make the consumer check expect the llama-server backend**

In `cmake/consumer-check/CMakeLists.txt`, replace

```cmake
# With Morpheus's vendored defaults nothing beyond the six engine libraries exists: no app, tests or tools, no
# app-model or app-ui library, and none of llama.cpp, SDL3, ImGui or Catch2.
```

with

```cmake
# With Morpheus's vendored defaults nothing beyond the seven engine libraries exists (the llama-server backend among
# them, with cpp-httplib's header): no app, tests or tools, no app-model or app-ui library, and none of llama.cpp,
# SDL3, ImGui or Catch2.
```

and after the `Morpheus::grammar` check add:

```cmake
if(NOT TARGET Morpheus::llamaserver)
    message(FATAL_ERROR "The vendored Morpheus build does not define Morpheus::llamaserver.")
endif()
```

Run: `cmake -S cmake/consumer-check -B build/consumer-check && cmake --build build/consumer-check --config Debug && ./build/consumer-check/Debug/consumer_check.exe`
Expected: configures (cpp-httplib is fetched, no unwanted target is defined, `Morpheus::llamaserver` exists), builds, and prints `consumer_check: ok (rolled ..., total ...)`.

- [ ] **Step 2: Update the architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, make these replacements. Each "Replace" text appears once in the file; "with" is the new text, exactly.

1. Replace

   ```text
   - **Local-first.** llama.cpp runs inside the engine process. A cloud provider can be added behind the same interface later.
   ```

   with

   ```text
   - **Local-first.** llama.cpp runs inside the engine process, or the models are reached through `llama-server` over HTTP (`settings.json`'s `backend`), so one model host can serve several consumers. A cloud provider can be added behind the same interface later.
   ```

2. Replace

   ```text
   - **C++20, one process, one executable.** No HTTP between modules, no router, no Ollama.
   ```

   with

   ```text
   - **C++20, one process, one executable.** No HTTP between modules, no router, no Ollama. The only HTTP is the optional llama-server backend's, to a model host the owner runs.
   ```

3. Replace

   ```text
   it calls the backend's `CheckGrammar` from the UI thread, only while the worker is blocked waiting for the seat.
   ```

   with

   ```text
   it calls `grammar::Check` and the backend's `CountTokens` from the UI thread, only while the worker is blocked waiting for the seat.
   ```

4. Replace the module table row

   ```text
   | `inference` | `LlmBackend` interface and prompt types (no llama.cpp) | core |
   ```

   with

   ```text
   | `inference` | The `Tokenizer`, `ChatModel` and `Embedder` interfaces, `LlmBackend` (their union), prompt types and the thinking block's texts (no llama.cpp, no HTTP) | - (only `core/Errors.h`, header-only) |
   ```

5. After the module table row that starts ``| `llama` |`` (the whole line), add the row

   ```text
   | `llamaserver` | `LlamaServerBackend`: the models through `llama-server` over HTTP (cpp-httplib, no OpenSSL): token counts, prompt rendering, streamed completions on a slot with prompt-cache reuse, embeddings. Built in every preset, behind `MORPHEUS_BUILD_LLAMA_SERVER` (default on) | inference, core |
   ```

6. In the row that starts ``| `grammar` |``, replace

   ```text
   and `Validate`, which checks a value against the same subset. No llama.cpp
   ```

   with

   ```text
   `Validate`, which checks a value against the same subset, and `grammar::Check`, which says whether a text fits a GBNF grammar and where it stops fitting. No llama.cpp
   ```

7. In the row that starts ``| `app` |``, replace

   ```text
   | turn, llama (`main.cpp`'s backend factory) |
   ```

   with

   ```text
   | turn, llama and llamaserver (`main.cpp`'s backend factories) |
   ```

8. Replace

   ```text
   Morpheus's own resolve, lookup and review grammars (`turn`) are built on the builder, not on JSON Schema.
   ```

   with

   ```text
   Morpheus's own resolve, lookup and review grammars (`turn`) are built on the builder, not on JSON Schema. `grammar::Check(grammar, text)` matches a text against a grammar by code point, as llama.cpp's grammar sampler does, memoised by (expression, position) so long texts stay fast; it returns "" or llama.cpp's reason texts ("the grammar rejects '<c>' at the start", "... after '<text>'", "the text ends before the grammar does"), pointing at the furthest position any partial match reached, and throws `ContractError` for a grammar it cannot read. The model seat checks replies with it; the `[llama]` tests check that it agrees with llama.cpp.
   ```

9. In the **Build:** paragraph, replace

   ```text
   the SQLite amalgamation, Catch2 v3.
   ```

   with

   ```text
   the SQLite amalgamation, Catch2 v3, cpp-httplib (header-only, without OpenSSL; Morpheus defines its own `morpheus_httplib` target).
   ```

   and replace

   ```text
   `Morpheus::grammar`, `Morpheus::turn`) with nlohmann/json (unless the host provides it) and SQLite (`morpheus_sqlite3`)
   ```

   with

   ```text
   `Morpheus::grammar`, `Morpheus::turn`, `Morpheus::llamaserver`) with nlohmann/json (unless the host provides it), SQLite (`morpheus_sqlite3`) and cpp-httplib's header (`morpheus_httplib`; `MORPHEUS_BUILD_LLAMA_SERVER` off leaves it and the llama-server backend out)
   ```

10. Replace the code block under `## Inference interface`

    ```text
    LlmBackend
      count_tokens(text) -> int
      count_prompt_tokens(prompt) -> int   (the full templated prompt, exactly as complete evaluates it without thinking)
      context_size() -> int
      complete(Prompt{messages, stableCount}, GenerationSpec{grammar?, max_tokens, thinking_max_tokens, sampling}, on_token, cancel)
          -> Completion{text, thinking, tokens_reused, tokens_evaluated, timings}
      embed(texts, purpose: query|document) -> vectors
      embedding_model_id() -> string
      save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
      check_grammar(grammar, text) -> "" or a reason   (whether text, tokenized as an answer, fits the grammar from start to end)
    ```

    with

    ```text
    Tokenizer
      count_tokens(text) -> int
      count_prompt_tokens(prompt) -> int   (the full templated prompt, exactly as complete evaluates it without thinking)
      context_size() -> int
    ChatModel
      complete(Prompt{messages, stableCount}, GenerationSpec{grammar?, max_tokens, thinking_max_tokens, sampling, slot}, on_token, cancel)
          -> Completion{text, thinking, tokens_reused, tokens_evaluated, timings}
      save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
    Embedder
      embed(texts, purpose: query|document) -> vectors
      embedding_model_id() -> string
    LlmBackend = Tokenizer + ChatModel + Embedder
    ```

    and add this bullet right after the code block, before the bullet that starts ``- `LlamaCppBackend` loads two GGUF models``:

    ```text
    - Callers that need the whole backend take `LlmBackend`; code that needs one part (sub-project D's server) can take `Tokenizer`, `ChatModel` or `Embedder` alone. `GenerationSpec::slot` names the server slot whose prompt cache a call uses: -1 (the default, Morpheus's own game) is the backend's default slot; the in-process backend has one cache and ignores it. `turn::SamplingFrom` builds a call's sampling from `settings.json`. The thinking block's texts (`inference::THINKING_OPEN_TEXT`, `THINKING_SKIP_TEXT`, `THINKING_END_TEXT`, `THINKING_AFTER_END_TEXT`, `CLOSE_THINKING_TEXT`) are shared by both backends.
    ```

11. Replace the whole bullet that starts ``- `CheckGrammar` generates nothing:`` and ends `The app's model seat uses it to check a hand-written reply.` with

    ```text
    - `LlamaCppBackend::CheckGrammar` (a plain method, not part of the interface) generates nothing: it tokenizes the text as an answer and runs llama.cpp's grammar sampler over it, checking each token alone and then accepting it, and at the end requires the end-of-generation token to be allowed. Its reasons have `grammar::Check`'s shape, naming the first rejected token instead of a code point. An empty or unparsable grammar is a `ContractError`. The `[llama]` tests use it to confirm that `grammar::Check`, which the app's model seat uses, agrees with llama.cpp.
    - `LlamaServerBackend` (`llamaserver`, design: `docs/superpowers/specs/2026-11-15-llama-server-backend-design.md`) reaches the models through `llama-server`. At startup it reads both servers' `GET /props`: the chat server's per-slot context must hold `context_size`, its chat template tells whether the model thinks, and the embedding server's model file names the vectors. It counts tokens with `POST /tokenize`, renders prompts with `POST /apply-template` around content markers and tokenizes each template piece (special tokens parsed) and each content (plain text) apart, as in-process (`inference::ContentMarker`, `MarkContents`, `TemplatePieces`), appending the thinking markers as tokens, and streams `POST /completion` with the prompt as token ids, `id_slot`, `cache_prompt` and the in-process sampling chain (`samplers` top_k, top_p, temperature; every other server sampler neutral); the server's `timings` fill the telemetry. A call that thinks is two calls on one slot: the block (stopping at `</think>` or its budget), then the answer, whose tokens extend the first call's (the thinking text tokenized again as plain text) so the server reuses its cache. A stop closes the connection and llama-server cancels the task. It embeds with `POST /embedding`, one text at a time. `SaveState` and `RestoreState` are not supported (`InferenceError`). One thread owns each backend; several may share a server, each on its own slot.
    ```

12. Replace

    ```text
    - `settings.json` holds the model paths, `n_ctx`, the number of GPU layers, sampling per call type,
    ```

    with

    ```text
    - `settings.json` holds the backend (`"llama-cpp"`: the model paths and the number of GPU layers; `"llama-server"`: a `server` object with `chat_url`, `embedding_url` and `slot`; each backend's keys are ignored under the other), `n_ctx`, sampling per call type,
    ```

13. Replace the UI table row

    ```text
    | Settings | Models (picked from `models/`), context size, thinking budgets (the review's included), sampling, `memory_mode`, narration check, compact transcript and prose turns |
    ```

    with

    ```text
    | Settings | Backend (llama.cpp in-process, or llama-server over HTTP); models (picked from `models/`) or the server URLs and slot; context size, thinking budgets (the review's included), sampling, `memory_mode`, narration check, compact transcript and prose turns |
    ```

14. Replace

    ```text
    Settings apply by restarting the worker: the loaded models are kept unless the model paths, embedding prefixes, context size or GPU layers changed, and the open run reopens.
    ```

    with

    ```text
    Settings apply by restarting the worker: the loaded backend is kept unless the backend kind, the model paths, the server URLs or slot, the embedding prefixes, the context size or GPU layers changed, and the open run reopens. `app::BackendFactoryByKind` makes the backend `settings.json` names.
    ```

15. Replace

    ```text
    The fast suite matches grammars with a test-only GBNF matcher (`tests/grammar/GbnfMatcher.h`); the `[llama]` suite checks the same values with llama.cpp's grammar sampler.
    ```

    with

    ```text
    The fast suite matches grammars with `grammar::Check` (whose own tests cover its reasons, its strict repetition counts and its memoisation); the `[llama]` suite checks the same values with llama.cpp's grammar sampler, and that both agree on accept, reject and the kind of reason.
    ```

16. After the bullet that starts ``- **`context` tests:**``, add the bullet

    ```text
    - **`llamaserver` tests:** a fake `llama-server` (`tests/fakes/FakeLlamaServer.h`, a cpp-httplib server on a free local port) answers from a script and records every request: startup, token counts, rendering and its thinking markers, the `/completion` body, streaming, telemetry, thinking in two calls, errors, cancel and embeddings. The `[server]` suite runs against a `llama-server` the owner starts (`MORPHEUS_TEST_SERVER_URL`, `MORPHEUS_TEST_EMBED_SERVER_URL`), including R2's three sessions on three slots, which reports each call's reused share of its prompt.
    ```

17. Replace

    ```text
    - Multiple processes, localhost HTTP, router/proxy, allowlists, aliases, and JSON pipeline templates
    ```

    with

    ```text
    - Multiple processes of Morpheus's own, a router/proxy, allowlists, aliases, and JSON pipeline templates (the llama-server backend's HTTP goes to a model host Morpheus does not run)
    ```

- [ ] **Step 3: Update the README**

In `README.md`:

1. Replace

   ```text
   The `dev` and `linux-release` presets run models in-process through llama.cpp. Download two GGUF files into `models/` (gitignored):
   ```

   with

   ```text
   The `dev` and `linux-release` presets run models in-process through llama.cpp, or reach them through `llama-server` over HTTP (`backend`, below). For llama.cpp in-process, download two GGUF files into `models/` (gitignored):
   ```

2. Right after the paragraph that ends `Relative model paths resolve against the settings file's folder.` (before the bullet that starts ``- `budget.retrieval_cap` ``), insert this bullet:

   ````text
   - `backend`: `"llama-cpp"` runs the models in-process and needs `chat_model`, `embedding_model` and `gpu_layers`; `"llama-server"` reaches them through two running `llama-server`s (see "Models through llama-server" below) and needs a `server` object instead (the model keys, if present, are ignored, as `server` is under `llama-cpp`):

     ```json
     "backend": "llama-server",
     "server": {
       "chat_url": "http://127.0.0.1:8080",
       "embedding_url": "http://127.0.0.1:8081",
       "slot": 0
     },
     ```

     The URLs are `http://host:port` (no HTTPS, no path); `slot` (at least 0) is the chat server's slot for Morpheus's calls. `context_size` is still what the budgets plan for: each of the chat server's slots must hold at least that many tokens, or the backend refuses to start.
   ````

3. Replace

   ```text
   with them every call evaluates its whole prompt, and the probe reports 0 reused.
   ```

   with

   ```text
   with them every call evaluates its whole prompt, and the probe reports 0 reused. That is in-process; llama-server keeps context checkpoints for such models, and what it reuses is measured below.
   ```

4. Right before the line `### Performance with Qwen3.8-27B on a 10 GB GPU`, insert:

   ````text
   ### Models through llama-server

   With `"backend": "llama-server"`, Morpheus runs no model itself: one model host can serve the app, `morpheus_play`, `morpheus_probe` and other programs at once, each on its own slot, and each slot keeps its own prompt cache. Morpheus does not build `llama-server`: use a llama.cpp release or your own build. Start a chat server and an embedding server, for example:

   ```
   llama-server -m models/Qwen3.8-27B-UD-Q4_K_M.gguf --host 127.0.0.1 --port 8080 --parallel 3 --ctx-size 73728 -ngl 24
   llama-server -m models/nomic-embed-text-v1.5.Q8_0.gguf --host 127.0.0.1 --port 8081 --embedding --ctx-size 2048 --batch-size 2048 --ubatch-size 2048
   ```

   - `--parallel N` gives the chat server N slots, each with `--ctx-size / N` tokens of context: make that at least `context_size` (73728 / 3 = 24576 above).
   - The embedding server needs `--embedding` and a pooled sentence-embedding model; a batch as large as its context lets a long text be embedded in one piece, as in-process.
   - Morpheus sends the prompt as token ids, tokenized as in-process (a player's text never becomes control tokens), and names the in-process sampling chain in every request (top-k, top-p, temperature; the server's other samplers neutral), so the server's command-line sampling options do not change Morpheus's calls.
   - Thinking works as in-process: Morpheus writes the thinking block's markers itself, so the thinking budgets apply unchanged.
   - `morpheus_probe` works with either backend; with llama-server its reuse figures are the server's, so the second run shows what a slot reuses.

   ### The [server] tests

   Tests tagged `[server]` run against your servers and skip unless both variables are set. They need a chat server with at least 3 slots of at least 16384 tokens (`--parallel 3 --ctx-size 49152`):

   ```
   export MORPHEUS_TEST_SERVER_URL=http://127.0.0.1:8080
   export MORPHEUS_TEST_EMBED_SERVER_URL=http://127.0.0.1:8081
   ./build/tests/bin/Debug/morpheus_tests.exe "[server]"
   ```

   They complete a resolve call under its grammar and a streamed narrate call, think on a thinking model (skipped on one without a thinking mode), and run three sessions on three slots at once, each alternating an action call and a speech call for ten exchanges; that test prints each call's reused share of its prompt (`tokensReused / promptTokens`) and checks that every call after a session's first reuses some. In the `dev` build, with the `[llama]` variables set too, `"[server][llama]"` checks that the server and the in-process backend count the same tokens when the server runs the `MORPHEUS_TEST_CHAT_MODEL` file.

   ### Prompt-cache reuse through llama-server

   Not measured yet. Run the three-slot `[server]` test with Qwen3-4B-Instruct and with a Qwen3.5 or later model, and record here the reused shares it prints (R2 of the Skyrim integration asks for more than 80% on the second and later calls of a session; for Qwen3.5 this measures what its linear-attention layers allow through llama-server).

   ````

5. Replace

   ```text
   `Morpheus::grammar` and `Morpheus::turn`, with nlohmann/json and SQLite (as `morpheus_sqlite3`, so it never meets a `sqlite3` target of yours).
   ```

   with

   ```text
   `Morpheus::grammar`, `Morpheus::turn` and `Morpheus::llamaserver` (the llama-server backend), with nlohmann/json, SQLite (as `morpheus_sqlite3`, so it never meets a `sqlite3` target of yours) and cpp-httplib's header (as `morpheus_httplib`, without OpenSSL); `set(MORPHEUS_BUILD_LLAMA_SERVER OFF)` leaves the llama-server backend and cpp-httplib out.
   ```

6. Replace

   ```text
   - **Settings:** the chat and embedding models, picked from the `.gguf` files in `models/`; context size, GPU layers,
   ```

   with

   ```text
   - **Settings:** the backend; under llama.cpp the chat and embedding models, picked from the `.gguf` files in `models/`, and GPU layers; under llama-server the two server URLs and the slot; context size,
   ```

   and replace

   ```text
   changing the models, the embedding prefixes, the context size or GPU layers reloads the models.
   ```

   with

   ```text
   changing the backend, the models, the server URLs or slot, the embedding prefixes, the context size or GPU layers reloads the backend.
   ```

7. In `## Layout`, replace

   ```text
   - `src/inference`: the `LlmBackend` interface (no llama.cpp)
   ```

   with

   ```text
   - `src/inference`: the `Tokenizer`, `ChatModel` and `Embedder` interfaces and `LlmBackend`, their union (no llama.cpp)
   ```

   replace

   ```text
   - `src/grammar`: bounded-JSON grammars: the GBNF builder, `FromSchema` and `Validate` (no llama.cpp)
   ```

   with

   ```text
   - `src/grammar`: bounded-JSON grammars: the GBNF builder, `FromSchema`, `Validate` and `grammar::Check` (no llama.cpp)
   ```

   and after the line that starts ``- `src/llama`: the llama.cpp backend`` add

   ```text
   - `src/llamaserver`: the llama-server backend over HTTP (cpp-httplib; every preset, `MORPHEUS_BUILD_LLAMA_SERVER`)
   ```

- [ ] **Step 4: Update CLAUDE.md**

In `CLAUDE.md`:

1. Replace

   ```text
   (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::grammar`, `Morpheus::turn`)
   ```

   with

   ```text
   (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::grammar`, `Morpheus::turn`, `Morpheus::llamaserver`)
   ```

2. After the line that starts ``- Model-backed tests are tagged `[llama]` ``, add

   ```text
   - Real-server tests are tagged `[server]` and skip unless `MORPHEUS_TEST_SERVER_URL` and `MORPHEUS_TEST_EMBED_SERVER_URL` point at running llama-servers (README, "The [server] tests").
   ```

3. Replace

   ```text
   - src/inference: the LlmBackend interface and its types. No llama.cpp.
   ```

   with

   ```text
   - src/inference: the Tokenizer, ChatModel and Embedder interfaces, LlmBackend (their union) and their types. No llama.cpp, no HTTP.
   ```

4. Replace

   ```text
   - src/grammar: bounded-JSON grammars: the GBNF builder, `FromSchema` and `Validate`. No llama.cpp.
   ```

   with

   ```text
   - src/grammar: bounded-JSON grammars: the GBNF builder, `FromSchema`, `Validate` and `grammar::Check` (does a text fit a grammar, and where it stops fitting). No llama.cpp.
   ```

5. After the line that starts `- src/llama: the llama.cpp backend`, add

   ```text
   - src/llamaserver: the llama-server backend over HTTP (cpp-httplib, header-only, no OpenSSL), built in every preset (`MORPHEUS_BUILD_LLAMA_SERVER`, default on). Only it and the tests include <httplib.h>; tests/fakes/FakeLlamaServer.h is the fast suite's fake server.
   ```

- [ ] **Step 5: Check the documentation's bytes and names**

```bash
check_bytes docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md CLAUDE.md cmake/consumer-check/CMakeLists.txt
grep -n "GbnfMatcher\|check_grammar\|backend's \`CheckGrammar\`" docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md CLAUDE.md
```

Expected: `check_bytes` prints nothing; the `grep` prints nothing.

- [ ] **Step 6: Run the whole fast suite and AddressSanitizer**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: `0 tests failed out of 412`; the three `[server]` tests "Not Run (Skipped)".

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `0 tests failed out of 412`, the same three skipped, no AddressSanitizer report.

- [ ] **Step 7: Build every dev target and run the model-backed tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run, with the two model variables exported as in Task 2: `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"`
Expected: `All tests passed`; the thinking tests and `[server][llama]` skipped as their variables say.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 3`
Expected: exits with 0.

- [ ] **Step 8: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md CLAUDE.md cmake/consumer-check/CMakeLists.txt
git commit -m "Document model access over HTTP: the backend split, grammar::Check, LlamaServerBackend, the settings keys" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git status --short
```

Expected: `git status --short` lists nothing of this plan (an untracked `.superpowers/` or the ignored `settings.json` are not staged and stay so).

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| `LlmBackend` split into `Tokenizer`, `ChatModel`, `Embedder`; `LlmBackend` their union; each method keeps its contract and comment; callers unchanged | 1 (union with `CheckGrammar` kept), 3 (`{}`) |
| `CheckGrammar` leaves the interface; every implementation drops its override | 3 |
| The model seat's Check calls `grammar::Check` | 3 |
| `LlamaCppBackend::CheckGrammar` stays as a plain public method with its behaviour | 3 |
| `RecordingBackend`, `ModelSeatBackend`, `FakeBackend`, `ScriptedBackend` lose `CheckGrammar`; the verdict hooks go | 3 |
| `GenerationSpec` gains `int slot = -1`; -1 the default slot; in-process ignores it; server sends `id_slot` | 1 (field, comment, test), 6 (`id_slot` default and explicit) |
| `SamplingFrom` moves to `turn::SamplingFrom`; `inference` loses its `core` dependency; module table updated | 1, 11 (table) |
| Settings `backend` (required), `server` with `chat_url`, `embedding_url`, `slot` (required under llama-server) | 7 |
| Model keys and `gpu_layers` required only under llama-cpp, ignored when present under llama-server | 7 |
| Prefixes, `context_size`, budget and sampling stay required for both; `context_size` is what the budgets check | 7; 5 (`ContextSize()` returns it) |
| Startup check that the server's per-slot context is at least `context_size` | 5 |
| Validation: unknown backend, missing `server` or key is a `DataError` naming file and key; `http://` prefix; slot >= 0 | 7 |
| `core::Settings` gains `backend` (`BackendKind { LlamaCpp, LlamaServer }`), `chatUrl`, `embeddingUrl`, `serverSlot` | 7 |
| `SettingsToJson` writes them; `settings.example.json` sets `"backend": "llama-cpp"`; README's server example | 7; 11 (README) |
| This machine's `settings.json` gains `"backend": "llama-cpp"`, never staged | 7 (Step 5) |
| Settings window edits `backend` and, under llama-server, the URLs and slot; model fields only under llama-cpp; `SettingsEdit` reads and writes the keys | 8 |
| The engine host rebuilds the backend when the kind, a URL or the slot changes | 9 |
| `grammar::Check` in `src/grammar/Check.h/.cpp`, signature and reason texts verbatim | 2 |
| Reason at the furthest partial match; rejected code point; "ends before" when every path consumed the text | 2 (Check names the character ...) |
| Repetition counts `digits`, `digits,`, `digits,digits`, min <= max, else a read error | 2 (well-formed repetition counts) |
| Memoised by (node, position); nested repetition over a 2,000-character text finishes quickly | 2 (stays fast ...) |
| Every read error is a `ContractError` | 2 (refuses grammars ...) |
| Not modelling llama.cpp's load-time refusals | 2 (`Check.h` comment) |
| Test files switch from `test::GbnfMatcher` to `grammar::Check`; the header is deleted | 2 (Step 5) |
| Existing matcher, brute-force and property tests run on it; new tests for reasons, hardening, memoisation | 2 |
| Model-backed agreement test: accept/reject and reason kind, on the random schema values and `IntRange` probes | 2 (Step 5, `LlamaGrammarTests.cpp`) |
| `llamaserver::LlamaServerBackend` in `src/llamaserver`, `morpheus_llamaserver`, `Morpheus::llamaserver`; depends on inference, core, nlohmann/json | 5 |
| Built in every preset behind `MORPHEUS_BUILD_LLAMA_SERVER` (default ON); cpp-httplib fetched pinned, no OpenSSL | 4, 5, 11 (consumer check) |
| `LlamaServerConfig { chatUrl, embeddingUrl, slot, contextSize, embeddingQueryPrefix, embeddingDocumentPrefix }`, built by the callers | 5, 9 |
| Startup reads `GET /props` of both servers: per-slot context, chat template, thinking switch; embedding model id | 5 |
| Per-slot context too small; unreachable server, HTTP error, unexpected response: `InferenceError` naming the URL | 5 |
| `CountTokens` with `/tokenize` (`add_special` false, `parse_special` false) | 5 |
| `CountPromptTokens` renders around content markers and tokenizes the template pieces with special tokens parsed (BOS on the first when the model adds one, never doubled) and the contents as plain text, as `LlamaCppBackend`; one token list | 5 |
| Player text never becomes control tokens: a content holding `<|im_end|>` is tokenized with `parse_special: false` | 5 (tokenizes message contents as plain text) |
| The marker selection is shared, not copied: `inference::ContentMarker`, `MarkContents`, `TemplatePieces`, used by both backends | 5 |
| Rendering with `/apply-template`, `add_generation_prompt: true`, `chat_template_kwargs.enable_thinking` for thinking templates; markers appended as tokens, as in-process | 5 |
| Rendering cached per prompt within one `Complete` call, not across calls | 6 (tokenized once at the top of `Complete`) |
| `Complete` without thinking: one streaming `/completion` with `prompt` (token ids), `n_predict`, `grammar`, sampling, `id_slot`, `cache_prompt`, `stream`; pieces to `onToken` | 6 |
| Sampling as in-process: `samplers` top_k, top_p, temperature (`min_keep` 1), every other llama-server sampler neutral | 6 (the request-body test) |
| With thinking: two calls; thinking call without grammar or `onToken`, stops at the close marker or the budget minus the closing sentence (appended at the limit); the answer call extends its tokens with the thinking text tokenized as plain text and the marker tokens | 6 |
| Telemetry from `timings`: `prompt_n`, `tokensReused`, `predicted_n`, `prompt_ms`, `predicted_ms`; the thinking call fills the thinking fields; `stoppedAtLimit` from `stop_type` | 6 |
| Cancel: the receiver returns false, the connection closes, `cancelled` set | 6 |
| Contract checks as `LlamaCppBackend` (`maxTokens <= 0`, thinking without a thinking mode, prompt plus generation over the context) | 6 |
| `Embed` posts each text with its prefix to `/embedding`, L2-normalises, one vector per text; `EmbeddingModelId` from `/props` | 5 |
| `SaveState`, `RestoreState` throw `InferenceError` ("not supported by the llama-server backend") | 5 |
| Threading: one owner thread; several backends on one server, each on its own slot | 5 (class comment), 10 (three threads, three slots) |
| Callers: app, `morpheus_play`, `morpheus_probe` build the backend from `settings.backend` | 9 |
| `EngineHost`'s keep rule compares the kind, URLs and slot | 9 |
| `morpheus_probe` on llama-server reports the server's reuse figures | 9 (`Completion` telemetry from timings; probe comment and backend line), 11 (README) |
| Fake `llama-server` (`tests/fakes/FakeLlamaServer.h`): free port, scripted `/props`, `/tokenize`, `/apply-template`, streamed `/completion`, `/embedding`, records requests | 4 |
| Fast tests: startup, token counts (pieces and contents apart, BOS, a repeated or dropped content), `Complete` body (token prompt, sampling), streaming, telemetry, `stoppedAtLimit`, HTTP 500 and malformed lines, thinking, cancel with the close seen by the fake, embeddings | 5, 6 |
| Fast tests: settings load and round-trip, each validation error names its key, llama-cpp still requires model keys, `SettingsEdit`, engine host rebuild | 7, 8, 9 |
| Callers test: the app's factory picks the backend per setting, through `AppController` with a recording factory | 9 |
| `[server]` suite skipped unless `MORPHEUS_TEST_SERVER_URL` is set; owner's server with 3 slots and a large enough context | 10 |
| Token counts agree with in-process for the same model | 10 (`[server][llama]`) |
| A turn's resolve and narrate calls with a grammar; thinking on a thinking model | 10 |
| R2: three sessions on three slots, ten exchanges of action and speech calls, 16k context, reused fraction reported and > 0; 80% not asserted | 10 |
| Model-backed (`[llama]`) agreement test | 2 |
| Docs: architecture spec (split, module row, `grammar::Check`, settings keys, `SamplingFrom` in turn, slot) | 11 |
| Docs: README (choosing the backend, starting llama-server with `--parallel`, per-slot context, `--embedding`, the `[server]` tests, the reuse measurement) | 11 (the figures are the owner's to record) |
| Docs: `CLAUDE.md` layout gains `src/llamaserver` | 11 |
| Out of scope: `morpheus_server`, per-conversation slots, HTTPS and authentication, a llama.cpp bump | not touched |

