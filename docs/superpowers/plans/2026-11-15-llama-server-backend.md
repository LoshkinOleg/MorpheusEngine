# Model Access over HTTP (llama-server) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Morpheus reaches its models through `llama-server` over HTTP as well as in-process: `inference::LlmBackend` is split into `Tokenizer`, `ChatModel` and `Embedder`, the grammar check becomes backend-independent (`grammar::Check`), a new library `morpheus_llamaserver` implements the backend with cpp-httplib, and `settings.json` chooses the backend for the app, `morpheus_play` and `morpheus_probe`.

**Architecture:**
- **Interfaces (`inference`):** besides the split, `inference` gains the content-marker helpers both backends use to keep message contents from becoming control tokens (`ContentMarker`, `MarkContents`, `TemplatePieces`, moved out of `LlamaCppBackend::TokenizePrompt`). `Tokenizer` (`CountTokens`, `CountPromptTokens`, `ContextSize`), `ChatModel` (`Complete`, `SaveState`, `RestoreState`), `Embedder` (`Embed`, `EmbeddingModelId`); `LlmBackend` is their union, so its callers do not change. `GenerationSpec` gains `int slot = -1`. `SamplingFrom` moves to `turn`, and `morpheus_inference` no longer links `morpheus_core`. The thinking block's texts (`THINKING_OPEN_TEXT`, ..., `CLOSE_THINKING_TEXT`) move from `LlamaCppBackend` into `inference`, so both backends write the same markers.
- **`grammar::Check` (`src/grammar/Check.h/.cpp`):** the test-only GBNF matcher, promoted: memoised by (node, position), strict repetition counts, a reason that points at the furthest position any partial match reached, and `ContractError` for every read error. The model seat's Check calls it; `CheckGrammar` leaves the interface and stays on `LlamaCppBackend` as a plain method, which the `[llama]` tests compare with `grammar::Check`.
- **`llamaserver::LlamaServerBackend` (`src/llamaserver`, `morpheus_llamaserver`, `Morpheus::llamaserver`):** reads both servers' `GET /props` at startup, counts with `POST /tokenize`, renders with `POST /apply-template` around content markers and tokenizes each template piece (special tokens parsed) and each content (plain text) apart, exactly as `LlamaCppBackend` does, appends the thinking markers as tokens, streams `POST /completion` with the prompt as token ids and the in-process sampling chain (two calls when the model thinks: the block, then the answer extending its tokens), embeds with `POST /embedding`. Built in every preset behind `MORPHEUS_BUILD_LLAMA_SERVER` (default ON); cpp-httplib is fetched pinned by commit, header-only, with no OpenSSL.
- **Settings (`core`):** `backend` (`"llama-cpp"` or `"llama-server"`), and under `llama-server` a `server` object (`chat_url`, `embedding_url`, `slot`). `core::Settings` gains `backend` (`core::BackendKind`), `chatUrl`, `embeddingUrl`, `serverSlot`.
- **Callers:** the app (`app::BackendFactoryByKind`), `morpheus_play` and `morpheus_probe` build the backend `settings.backend` names; the engine host rebuilds the backend when the kind, a URL or the slot changes.
- **Testing without a server:** `tests/fakes/FakeLlamaServer.h`, a cpp-httplib server on a free local port that answers from a script and records every request. The `[server]` suite runs against a `llama-server` the owner starts (`MORPHEUS_TEST_SERVER_URL`, `MORPHEUS_TEST_EMBED_SERVER_URL`). **No `llama-server` exists on this machine: the `[server]` tests are skipped here, and the plan never expects them to run.**
- **Spec:** `docs/superpowers/specs/2026-11-15-llama-server-backend-design.md` (the single source of requirements); requirement R2, option (a), of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md`; architecture spec `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`.

**Tech Stack:** C++20 with MSVC (VS 2022), CMake 3.25+, nlohmann/json 3.12.0, Catch2 v3, cpp-httplib v0.59.0 (new, header-only, MIT), llama.cpp b9804 (dev preset, for the `[llama]` tests). The llama-server API was read from the pinned llama.cpp source, `build/dev/_deps/llama-src/tools/server/` (see "llama-server facts" below).

**Choices this plan makes where the spec is silent, ambiguous or cannot be followed literally (the owner may overrule any of them):**
- **`SamplingFrom` has a second caller.** The spec says its only callers are in `turn/Session.cpp`; `tools/probe/main.cpp` calls it too. It becomes `turn::SamplingFrom`, declared in `turn/Session.h`, and `morpheus_probe` links `morpheus_turn`.
- **`inference` keeps one header from `core`.** `inference::RoleName` throws `core::ContractError`, the project's error for engine bugs. `morpheus_inference` stops linking `morpheus_core` and keeps including the header-only `core/Errors.h`; `morpheus_llama`, which used `core` through `inference`, links `morpheus_core` itself. The architecture table says `inference` depends on "- (only `core/Errors.h`, header-only)".
- **The thinking texts move to `inference`.** `LlamaCppBackend::CLOSE_THINKING_TEXT` and the marker literals become `inference::THINKING_OPEN_TEXT`, `THINKING_SKIP_TEXT`, `THINKING_END_TEXT`, `THINKING_AFTER_END_TEXT` and `CLOSE_THINKING_TEXT` (`std::string_view`), so "exactly as `LlamaCppBackend` does" is one definition.
- **The prompt goes as token ids, made as in-process (owner decision).** `LlamaServerBackend::TokenizePrompt` renders the prompt with `POST /apply-template` around the per-call content marker, cuts the text at the markers, and sends each template piece to `POST /tokenize` with `parse_special: true` (with `add_special` on the first piece only, when the model adds BOS; a BOS the template wrote itself is not doubled) and each content with `parse_special: false`; the ids are concatenated, the thinking markers appended as ids, and `/completion` gets that array as `prompt` (a prompt of ids is evaluated as it is: llama-server adds BOS only to a text prompt, README `/completion` `prompt`). `CountPromptTokens` is that list's length. Whether the model adds BOS is learnt at startup by tokenizing an empty text with `add_special`.
- **The content markers are shared.** `ContentMarker`, `MarkContents` and `TemplatePieces` move from `LlamaCppBackend::TokenizePrompt` into `inference` (Task 5), with two callers each; `LlamaCppBackend`'s behaviour and error texts are unchanged.
- **A prompt costs one `/tokenize` request per non-empty template piece and per non-empty content** (about 2n + 1 for n messages), each a localhost round trip: `/tokenize` takes one `parse_special` per request, so the pieces cannot be batched without losing their boundaries.
- **After thinking, the thinking text is tokenized again.** The answer call's prompt is the first call's ids, then `/tokenize` of the thinking text (`add_special` and `parse_special` false), then the end marker and blank line, or the closing sentence, as ids. The stream cannot give the generated ids: llama-server sends no message for a token that ends inside a UTF-8 character (`process_token` in `server-context.cpp` skips `send_partial_response` while the text is `incomplete`), and in stream mode the last message's `tokens` is emptied (`send_final_response`); a non-streamed call with `return_tokens` would keep a long thinking block silent past the read timeout. Where the model's own split differs from the canonical one, the server's cache reuse stops at that point.
- **A template that opens the thinking block itself** (ends with `<think>\n` when `enable_thinking` is true) has that marker dropped before the backend appends its own, so the block is never opened twice.
- **The thinking call stops at the close marker with `"stop": ["</think>"]`.** llama-server ends with `stop_type` `"word"` and leaves the stop word out of the text; the backend writes `</think>` and the blank line back. This relies on `</think>` being rendered as text, which holds for Qwen3 and later (their think markers are not control tokens); the `[server]` thinking test confirms it on a real server.
- **Thinking telemetry:** `thinkingTokens` is the thinking call's `timings.predicted_n` plus the tokens of the text the backend appends (the blank line after `</think>`, or the closing sentence); `thinkingMs` is the thinking call's `predicted_ms` plus the answer call's `prompt_ms`; `promptMs`, `tokensEvaluated` and `tokensReused` come from the first call. `tokensGenerated` is `timings.predicted_n`, which counts the end-of-generation token llama-server samples (the in-process count does not).
- **An extra fail-fast check:** the server's `tokens_evaluated` must equal the number of ids sent, or `Complete` throws `InferenceError` naming both numbers. The budgets rely on that count.
- **Sampling as in-process (owner decision).** `LlamaCppBackend`'s chain is greedy at temperature <= 0, otherwise top-k, top-p (min_keep 1), temperature, then a draw with the seed. llama-server builds its chain from `samplers` in order and always ends with a draw (`common_sampler_init` in `common/sampling.cpp`; at temperature 0 its temperature sampler keeps only the most likely token, so the draw is greedy). The request sends `samplers: ["top_k", "top_p", "temperature"]`, `min_keep: 1`, `dynatemp_range: 0`, `dynatemp_exponent: 1`, and neutral values for every sampler of the server's default list (`common_params_sampling::samplers` in `common/common.h`: penalties, dry, top_n_sigma, top_k, typ_p, top_p, min_p, xtc, temperature) and for what works outside the list: `repeat_penalty: 1`, `repeat_last_n: 0`, `frequency_penalty: 0`, `presence_penalty: 0`, `dry_multiplier: 0`, `top_n_sigma: -1`, `typical_p: 1`, `min_p: 0`, `xtc_probability: 0`, `mirostat: 0`, `logit_bias: []`, `ignore_eos: false`, `n_probs: 0` (names from `make_llama_cmpl_schema` in `tools/server/server-schema.cpp`). Request values override the server's command-line defaults (`eval_llama_cmpl_schema` starts from `params_base.sampling`). Same seed, same model: the in-process backend's choice of tokens, up to floating-point differences between batch sizes.
- **A grammar llama-server cannot parse** comes back as its HTTP 400, an `InferenceError`, not the in-process `ContractError`; the spec's contract list does not include it.
- **`EmbeddingModelId()`** is the file name of the embedding server's `/props` `model_path` without its extension (`/props` has no model id field; `model_alias` defaults to the full file name). It equals the in-process id (`stem()`), so a run's stored embeddings stay valid when the backend changes.
- **Cancel closes the connection at once** with a `std::stop_callback` that calls `httplib::Client::stop()`, besides the stream receiver returning false: a stop during a long prompt read (no bytes arrive yet) is then honoured without waiting for the first piece.
- **URLs:** settings require the `http://` prefix (the spec); the backend's constructor also refuses a URL with a path, query or empty host (`InferenceError`), because cpp-httplib would silently drop a path.
- **Each backend's keys are ignored under the other:** under `llama-server` the model keys and `gpu_layers` are not read or checked (the spec); under `llama-cpp` a `server` object is accepted and not checked. `SettingsToJson` writes only the active backend's keys; `SettingsEdit` writes only the active backend's keys and keeps the other's, so switching back and forth in the window loses nothing.
- **`MORPHEUS_BUILD_LLAMA` needs `MORPHEUS_BUILD_LLAMA_SERVER`** (a configure error otherwise): the app and both tools link both backends.
- **cpp-httplib's own `CMakeLists.txt` is not used.** It looks for OpenSSL, zlib, brotli and zstd by default; `SOURCE_SUBDIR` names a folder without one, so FetchContent only downloads, and the root `CMakeLists.txt` defines `morpheus_httplib` (INTERFACE, SYSTEM include), a name that never meets llama.cpp's vendored `cpp-httplib` target or a host's `httplib` target.
- **The app's backend factory** is `app::BackendFactoryByKind(llamaCpp, llamaServer)` in `EngineHost.h` (`morpheus_app_model` cannot link llama.cpp); `main.cpp` passes the two real factories, the test passes recording ones.
- **`grammar::Check` on text that is not UTF-8:** a byte that does not start a UTF-8 character is a character no expression matches; the reason quotes that byte.
- **Agreement "reason kind"** has three kinds: fits, rejects (both "at the start" and "after"), ends early. llama.cpp quotes a token and `grammar::Check` a code point, so where "at the start" falls can differ.
- **The `[server]` tests:** the R2 test runs the three sessions on three threads (each with its own backend and slot) and records results for the main thread to check (Catch2's checks are not thread-safe); the token-agreement test is skipped unless the server's model file name equals `MORPHEUS_TEST_CHAT_MODEL`'s; the thinking test is skipped when the server's model has no thinking mode. `EnvironmentVariable` moves to `tests/TestEnvironment.h`, since the tests preset (no llama.cpp) needs it.
- **Between Task 5 and Task 6** `LlamaServerBackend::Complete` throws `InferenceError("LlamaServerBackend::Complete is not implemented yet")`; Task 6 replaces it. Nothing calls it in between.
- **Timeouts:** 5 s to connect, 600 s to wait for the server's next bytes (a long prompt is read before the first piece arrives). Constants, not settings.
- **`stop_type` / `limit`:** the spec's "`stop_type` / `limit` flags" are one field: `stop_type` is `"limit"` when `n_predict` ran out.

## llama-server facts (pinned source, `build/dev/_deps/llama-src/tools/server/`)

- `GET /props` (`server-context.cpp`, `get_props`): `default_generation_settings.n_ctx` is the per-slot context (`meta->slot_n_ctx`, the context size divided by `--parallel` unless `--kv-unified`); `chat_template` is the default template's source; `model_path` is the model file; `total_slots` is `--parallel`.
- `POST /tokenize` (`post_tokenize`): `content` (string), `add_special` (default false), `parse_special` (default true); answers `{"tokens": [ids...]}`.
- `POST /apply-template` (`post_apply_template`, through `oaicompat_chat_params_parse` in `server-common.cpp`): `messages`, `add_generation_prompt` (default true), `chat_template_kwargs` (each value passed to the template; `enable_thinking` must be a boolean); answers `{"prompt": "..."}`.
- `POST /completion` (`post_completions`, `handle_completions_impl`): `prompt` may be a string (tokenized with `add_special` and `parse_special` both true) or an array of token ids, evaluated as given, without BOS (`tokenize_input_prompts`, `tokenize_mixed` in `server-common.cpp`; README `/completion` `prompt`); `id_slot` (-1: any idle slot), `cache_prompt` (default true), `stream`, `n_predict`, `grammar`, `stop`, `temperature`, `top_k`, `top_p`, `seed`. An error before streaming is an HTTP error with `{"error": {...}}`. Streaming (`format_oai_sse`) sends `data: <json>\n\n` per message, `:\n\n` pings, and `data: {"error": {...}}\n\n` for an error mid-stream; no `[DONE]` for `/completion`. A partial message (`server_task_result_cmpl_partial::to_json_non_oaicompat`) has `content`, `tokens`, `stop: false`, `id_slot`, `tokens_predicted`, `tokens_evaluated` (the prompt's tokens). The last message (`server_task_result_cmpl_final::to_json_non_oaicompat`) has `content: ""` in stream mode, `stop: true`, `stop_type` (`"eos"`, `"limit"`, `"word"`), `tokens_evaluated`, `tokens_cached`, `truncated` and `timings` (`cache_n`, `prompt_n`, `prompt_ms`, `predicted_n`, `predicted_ms`, ...). A closed connection makes `req.should_stop()` true and the task is cancelled. A partial message is sent per generated token except one that ends inside a UTF-8 character (`process_token`), and in stream mode the last message's `tokens` is empty (`send_final_response`), so the stream does not give every generated id.
- Sampling (`tools/server/server-schema.cpp`, `make_llama_cmpl_schema`; `common/sampling.cpp`, `common_sampler_init`): a request's fields override the server's command-line sampling (`eval_llama_cmpl_schema` copies `params_base.sampling` first). The chain is the optional logit bias, then the `samplers` list in order (`top_k`, `top_p` with `min_keep`, `temperature` as `temp_ext` with `dynatemp_range` and `dynatemp_exponent`, `penalties`, `dry`, `top_n_sigma`, `typ_p`, `min_p`, `xtc`, ...), then the draw with `seed`, unless `mirostat` is 1 or 2, which replaces the list. The default list (`common/common.h`) is penalties, dry, top_n_sigma, top_k, typ_p, top_p, min_p, xtc, temperature.
- `POST /embedding` (`post_embeddings`, `handle_embeddings_impl` with `TASK_RESPONSE_TYPE_NONE`): `content`; answers `[{"index": 0, "embedding": [[...floats...]]}]`, one vector per result when the model pools (L2-normalised by default, `embd_normalize` 2), one per token without pooling. Needs a server started with `--embedding`.

## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25. One new dependency: cpp-httplib v0.59.0, commit `cf3693cb5cc0d39b0e6f4122ba89bda9edb5ac6b` (tag `v0.59.0`, released 2026-10-03), header-only, no OpenSSL, SYSTEM includes.
- **Flags:** every MorpheusEngine target calls `morpheus_target_defaults()` (`/W4 /permissive- /Zc:preprocessor /utf-8`, warnings as errors at top level): a warning fails the build. `<httplib.h>` is only ever reached through the SYSTEM include of `morpheus_httplib`, so its own warnings are not ours.
- **Exact names (from the spec, verbatim):**
  - `inference::Tokenizer` (`CountTokens`, `CountPromptTokens`, `ContextSize`), `inference::ChatModel` (`Complete`, `SaveState`, `RestoreState`), `inference::Embedder` (`Embed`, `EmbeddingModelId`), `class LlmBackend : public Tokenizer, public ChatModel, public Embedder {};`.
  - `GenerationSpec::slot` (`int slot = -1;`): -1 is the backend's default slot; the in-process backend ignores it; `LlamaServerBackend` sends it as `id_slot`.
  - `turn::SamplingFrom(const core::SamplingSettings&, std::uint32_t)`.
  - `grammar::Check` in `src/grammar/Check.h/.cpp`: `std::string Check(const std::string& grammar, std::string_view text);` with the reasons `"the grammar rejects '<c>' at the start"`, `"the grammar rejects '<c>' after '<text>'"` (the text cut to its last 60 bytes, with `"..."` in front when cut) and `"the text ends before the grammar does"`; `""` when it fits.
  - `LlamaCppBackend::CheckGrammar` stays as a plain public method `std::string CheckGrammar(const std::string& grammar, std::string_view text)`.
  - `llamaserver::LlamaServerBackend` in `src/llamaserver`, library `morpheus_llamaserver`, alias `Morpheus::llamaserver`; `LlamaServerConfig { chatUrl, embeddingUrl, slot, contextSize, embeddingQueryPrefix, embeddingDocumentPrefix }`; option `MORPHEUS_BUILD_LLAMA_SERVER` (default ON).
  - Settings keys `backend` (`"llama-cpp"` or `"llama-server"`), `server.chat_url`, `server.embedding_url`, `server.slot`; `core::BackendKind { LlamaCpp, LlamaServer }`; `core::Settings` fields `backend`, `chatUrl`, `embeddingUrl`, `serverSlot`.
  - `tests/fakes/FakeLlamaServer.h`; `[server]` tests with `MORPHEUS_TEST_SERVER_URL` and `MORPHEUS_TEST_EMBED_SERVER_URL`.
  - `SaveState` / `RestoreState` messages contain "not supported by the llama-server backend".
- **Dependencies flow downward:** `morpheus_llamaserver` links `morpheus_inference` (PUBLIC), `morpheus_core`, `nlohmann_json::nlohmann_json` and `morpheus_httplib` (PRIVATE). No llama.cpp. `morpheus_inference` links nothing. Only `src/llamaserver/LlamaServerBackend.cpp` and test files include `<httplib.h>`, always behind `#define WIN32_LEAN_AND_MEAN` guarded by `#ifndef`.
- **Error policy:** engine bugs throw `core::ContractError` via `MORPHEUS_REQUIRE`; malformed `settings.json` throws `core::DataError` naming the file and the key; a server that cannot be reached, answers an HTTP error or answers something unexpected is an `inference::InferenceError` naming the URL (and path) and what was wrong. No fallbacks that hide a broken contract.
- **Coding rules (`CLAUDE.md`):** types and methods PascalCase, locals and parameters camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; constants ALL_CAPS and grouped; members initialized at declaration; class layout: nested types, public data, private data, public methods, private methods, helpers; comments let a method read top to bottom; a private helper only with more than one caller (recursive helpers count their recursion; local lambdas are fine); no legacy paths. ASCII only in source comments and docs; non-ASCII test text is written as `\x` escapes.
- **Never name a local variable or parameter `grammar`** in a scope that uses the `grammar::` namespace (the name would hide the namespace). The spec's `grammar::Check(const std::string& grammar, ...)` is the one exception: its body never writes `grammar::`.
- **Tests first:** every behaviour lands with a Catch2 test written and run (failing) before the implementation. Stated exceptions: the mechanical switch of existing tests from the matcher to `grammar::Check` (Task 2), the UI draw smoke test (Task 8), and the `[server]` tests (Task 10), which cannot run on this machine.
- **Byte check (required before every commit):** implementers' file tools have silently decoded `\u` and `\x` escapes into raw bytes before. Before each commit, run this on every file the commit names; it must print nothing (0 non-ASCII bytes, 0 control bytes other than tab, LF and the CR of a CRLF):

  ```bash
  check_bytes() { for f in "$@"; do [ -f "$f" ] && perl -ne 'print "$ARGV:$.: non-ASCII or control byte\n" if /[^\x09\x0A\x0D\x20-\x7E]/ || /\r(?!\n)/; close ARGV if eof' "$f"; done; }
  ```

  (`check_bytes file1 file2 ...`.) A file that prints a line has a raw byte where an escape was meant: rewrite that line with the escape.
- **Shell:** commands run from `D:\MorpheusEngine` in Git Bash:
  - `cmake --build --preset tests`, `ctest --preset tests` (368 tests pass before this plan); one tag: `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`.
  - The `dev` tree is configured in `build/dev` (RelWithDebInfo): `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`.
  - `[llama]` tests: `export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"; export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"`, then `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "<test names or tags>"`.
  - New test files and `CMakeLists.txt` changes are picked up by the next build (`CONFIGURE_DEPENDS`; CMake re-runs itself when a `CMakeLists.txt` changes). A new option or FetchContent needs no manual reconfigure.
  - Skipped tests (Catch2 `SKIP`) show in `ctest` as "Not Run (Skipped)" and are not failures.
- **Never stage** `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/`. Stage with the explicit paths each commit step names, nothing else (`git add -A` is forbidden).
- **Commits** are made on `master`. Every commit message ends with a `Co-Authored-By:` line naming the implementing model. The steps show `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; an implementer running as Opus writes `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` instead.

## File Structure

```
src/inference/Llm.h, Llm.cpp, CMakeLists.txt     modify: the split, GenerationSpec::slot, no SamplingFrom, no core link (Task 1);
                                                 CheckGrammar leaves LlmBackend (Task 3); the thinking texts and the content-marker
                                                 helpers (Task 5)
src/turn/Session.h, Session.cpp                  modify: turn::SamplingFrom (Task 1)
src/llama/CMakeLists.txt                         modify: links morpheus_core itself (Task 1)
tools/probe/main.cpp, tools/probe/CMakeLists.txt modify: turn::SamplingFrom (Task 1); the backend from settings (Task 9)
tests/inference/LlmTests.cpp                     modify: the split and the slot (Task 1); the content markers (Task 5)
tests/inference/SamplingTests.cpp                delete -> tests/turn/SamplingTests.cpp (Task 1)
src/grammar/Check.h, Check.cpp, CMakeLists.txt   create / modify: grammar::Check (Task 2)
tests/grammar/CheckTests.cpp                     create: grammar::Check's tests (Task 2)
tests/grammar/GbnfMatcher.h, GbnfMatcherTests.cpp delete (Task 2)
tests/grammar/GbnfTests.cpp, SchemaTests.cpp, PropertyTests.cpp,
tests/turn/ResolveGrammarTests.cpp               modify: test::GbnfMatcher -> grammar::Check (Task 2)
tests/llama/LlamaGrammarTests.cpp                modify: grammar::Check agrees with llama.cpp (Task 2)
src/llama/LlamaCppBackend.h, .cpp                modify: CheckGrammar a plain method (Task 3); the thinking texts and content markers
                                                 from inference, TokenizePrompt on them (Task 5)
src/turn/Replay.h, Replay.cpp                    modify: RecordingBackend drops CheckGrammar (Task 3)
src/app/model/ModelSeat.h, .cpp, ModelSeatBackend.h, .cpp, src/app/CMakeLists.txt
                                                 modify: Check uses grammar::Check (Task 3)
tests/fakes/FakeBackend.h, ScriptedBackend.h     modify: no CheckGrammar, no verdict hooks (Task 3)
tests/app/ModelSeatTests.cpp, ModelSeatBackendTests.cpp, UiTests.cpp, tests/turn/ReplayTests.cpp
                                                 modify (Task 3)
CMakeLists.txt                                   modify: MORPHEUS_BUILD_LLAMA_SERVER, cpp-httplib, morpheus_httplib (Task 4);
                                                 add_subdirectory(src/llamaserver) (Task 5)
tests/CMakeLists.txt                             modify: tests/llamaserver, morpheus_httplib (Task 4), morpheus_llamaserver (Task 5)
tests/fakes/FakeLlamaServer.h                    create: the fake llama-server (Task 4)
tests/llamaserver/FakeLlamaServerTests.cpp       create (Task 4)
src/llamaserver/LlamaServerBackend.h, .cpp, CMakeLists.txt
                                                 create: startup, prompt tokens, embeddings, state (Task 5); Complete (Task 6)
tests/llamaserver/ServerTestSupport.h            create: config and prompts for the fake-server tests (Task 5)
tests/llamaserver/LlamaServerBackendTests.cpp    create (Task 5)
tests/llamaserver/LlamaServerCompleteTests.cpp   create (Task 6)
src/core/Settings.h, Settings.cpp, settings.example.json, tests/core/SettingsTests.cpp
                                                 modify: the backend keys (Task 7)
settings.json                                    this machine only, never staged: gains "backend": "llama-cpp" (Task 7)
src/app/model/SettingsEdit.h, .cpp, src/app/ui/SettingsWindow.cpp, tests/app/SettingsEditTests.cpp
                                                 modify: the backend keys in the window (Task 8); UiTests.cpp (Task 8)
src/app/model/EngineHost.h, .cpp, src/app/main.cpp, tools/play/main.cpp, tools/play/CMakeLists.txt,
src/app/CMakeLists.txt, tests/app/EngineHostTests.cpp, tests/app/AppControllerTests.cpp
                                                 modify: the callers (Task 9)
tests/TestEnvironment.h                          create: EnvironmentVariable, moved from LlamaTestModels.h (Task 10)
tests/llama/LlamaTestModels.h                    modify: uses TestEnvironment.h (Task 10)
tests/llamaserver/LiveServer.h, LlamaServerLiveTests.cpp   create: the [server] suite (Task 10)
tests/llama/LlamaServerAgreementTests.cpp        create: [server][llama] token agreement (Task 10)
docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md, README.md, CLAUDE.md,
cmake/consumer-check/CMakeLists.txt              modify: documentation (Task 11)
```

---

### Task 1: Split the interface, add the slot, move SamplingFrom to turn

`LlmBackend` becomes the union of three interfaces. `CheckGrammar` stays on `LlmBackend` for now (Task 3 removes it), so no implementation changes in this task. `SamplingFrom` moves to `turn`, which takes `inference`'s last use of `core::Settings` away.

**Files:**
- Modify: `src/inference/Llm.h`, `src/inference/Llm.cpp`, `src/inference/CMakeLists.txt`
- Modify: `src/llama/CMakeLists.txt`
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`
- Modify: `tools/probe/main.cpp`, `tools/probe/CMakeLists.txt`
- Modify: `tests/inference/LlmTests.cpp`
- Delete: `tests/inference/SamplingTests.cpp`; Create: `tests/turn/SamplingTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `inference::Tokenizer`, `inference::ChatModel`, `inference::Embedder` (pure virtual, as below); `class inference::LlmBackend : public Tokenizer, public ChatModel, public Embedder` (holding only `CheckGrammar` until Task 3); `int inference::GenerationSpec::slot = -1`; `inference::Sampling turn::SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)` in `turn/Session.h`. `inference/Llm.h` no longer includes `core/Settings.h`.

- [ ] **Step 1: Write the failing tests**

In `tests/inference/LlmTests.cpp`, add `#include <type_traits>` after `#include <catch2/catch_test_macros.hpp>` (with a blank line between, as the other files group standard headers), and append:

```cpp
TEST_CASE("LlmBackend is the union of Tokenizer, ChatModel and Embedder", "[inference]")
{
    static_assert(std::is_base_of_v<Tokenizer, LlmBackend>);
    static_assert(std::is_base_of_v<ChatModel, LlmBackend>);
    static_assert(std::is_base_of_v<Embedder, LlmBackend>);

    // A caller can hold one part alone, as sub-project D's code will.
    morpheus::test::FakeBackend fake(1000);
    Tokenizer& tokenizer = fake;
    CHECK(tokenizer.CountTokens("abcde") == 2);
    CHECK(tokenizer.ContextSize() == 1000);
    ChatModel& chat = fake;
    CHECK_THROWS_AS(chat.SaveState(), morpheus::core::ContractError);
    Embedder& embedder = fake;
    CHECK_THROWS_AS(embedder.EmbeddingModelId(), morpheus::core::ContractError);
}

TEST_CASE("GenerationSpec's slot defaults to the backend's default slot", "[inference]")
{
    CHECK(GenerationSpec{}.slot == -1);
}
```

Delete the old sampling test and create its replacement in `turn`:

```bash
git rm -q tests/inference/SamplingTests.cpp
```

Create `tests/turn/SamplingTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "core/Settings.h"
#include "turn/Session.h"

using namespace morpheus;

TEST_CASE("SamplingFrom converts settings and carries the seed", "[turn]")
{
    const core::SamplingSettings settings = {0.25, 30, 0.5};
    const inference::Sampling sampling = turn::SamplingFrom(settings, 77);
    CHECK(sampling.temperature == 0.25f);
    CHECK(sampling.topK == 30);
    CHECK(sampling.topP == 0.5f);
    CHECK(sampling.seed == 77u);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `'Tokenizer': undeclared identifier` in `tests/inference/LlmTests.cpp`, `'slot': is not a member of 'morpheus::inference::GenerationSpec'`, and `'SamplingFrom': is not a member of 'morpheus::turn'` in `tests/turn/SamplingTests.cpp`.

- [ ] **Step 3: Split the interface**

Replace the whole of `src/inference/Llm.h` with:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace morpheus::inference {

// A model or runtime failure: a model file that will not load, a prompt that does not fit, a failed decode.
class InferenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Role { System, User, Assistant };

// Role spelling used by chat templates: "system", "user", "assistant".
std::string RoleName(Role role);

struct Message {
    Role role = Role::User;
    std::string content = {};
};

// A chat prompt. messages[0, stableCount) is the stable prefix (layers L0-L3); the rest is the volatile tail.
// The llama.cpp backend reuses the KV cache by longest common token prefix, so stableCount is informational for
// it; a cloud backend places its prompt-cache breakpoint there.
struct Prompt {
    std::vector<Message> messages = {};
    std::size_t stableCount = 0;
};

struct Sampling {
    float temperature = 0.0f; // <= 0 means greedy decoding.
    int topK = 40;
    float topP = 0.95f;
    std::uint32_t seed = 0;
};

struct GenerationSpec {
    std::string grammar = {}; // GBNF with a "root" rule; empty means unconstrained. Applies to the answer only.
    int maxTokens = 256;      // The answer: the text after any thinking.
    // > 0 lets a thinking model reason before it answers, in a block of at most this many tokens (its markers
    // included). The block is unconstrained, never streamed and kept out of Completion::text. 0: no thinking.
    int thinkingMaxTokens = 0;
    Sampling sampling = {};
    // The server slot whose prompt cache the call uses; -1 is the backend's default slot. The in-process backend has
    // one cache and ignores it; LlamaServerBackend sends it as id_slot. Morpheus's own game leaves it at -1.
    int slot = -1;
};

struct Completion {
    std::string text = {};     // The answer. Empty when the model ended its reply inside the thinking block.
    std::string thinking = {}; // The reasoning written in the thinking block, without its markers.
    int thinkingTokens = 0;    // Tokens the thinking block took, markers included.
    bool thinkingStoppedAtLimit = false; // The block reached thinkingMaxTokens and the backend closed it.
    double thinkingMs = 0.0;
    int promptTokens = 0;     // Tokens in the templated prompt.
    int tokensReused = 0;     // Prompt tokens served from the KV cache.
    int tokensEvaluated = 0;  // Prompt tokens computed for this call.
    int tokensGenerated = 0;     // Answer tokens.
    bool stoppedAtLimit = false; // The answer reached maxTokens before an end-of-generation token.
    bool cancelled = false;
    double promptMs = 0.0;
    double generateMs = 0.0; // The answer; thinkingMs covers the thinking block.
};

enum class EmbedPurpose { Query, Document };

// Receives generated text as it streams. Pieces never split a UTF-8 character.
using TokenCallback = std::function<void(std::string_view piece)>;

// Counts tokens as the chat model reads them (architecture spec, "Inference interface").
class Tokenizer {
public:
    virtual ~Tokenizer() = default;

    // Tokens in raw text: no chat template, no special tokens.
    virtual int CountTokens(std::string_view text) = 0;

    // Tokens in the full templated prompt, exactly as Complete would evaluate it without thinking. A call with
    // thinking evaluates no more than that: the markers of its thinking block count against thinkingMaxTokens.
    virtual int CountPromptTokens(const Prompt& prompt) = 0;

    virtual int ContextSize() const = 0;
};

// Generates the model's replies, and holds the chat cache they reuse.
class ChatModel {
public:
    virtual ~ChatModel() = default;

    // Generates the assistant's reply to `prompt`: with spec.thinkingMaxTokens > 0 a thinking block first, then the
    // answer, which alone is streamed to onToken and constrained by spec.grammar. Throws InferenceError if prompt +
    // spec.thinkingMaxTokens + spec.maxTokens exceeds the context, or if thinking is asked of a model without a
    // thinking mode. Throws ContractError for maxTokens <= 0, a negative thinkingMaxTokens, one too small to close
    // the block, or a grammar that does not parse. Cancellation through `stop` returns early with
    // completion.cancelled set; it is not an error.
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;

    // The chat KV cache and the tokens it holds, so the GPU can be handed to another model and the cache restored.
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(std::span<const std::uint8_t> state) = 0;
};

// Turns texts into vectors for retrieval.
class Embedder {
public:
    virtual ~Embedder() = default;

    // One L2-normalized vector per text, in order.
    virtual std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, EmbedPurpose purpose) = 0;

    // Identifies the embedding model, for store::EmbeddingRow::model.
    virtual std::string EmbeddingModelId() const = 0;
};

// The engine's view of its models: all three parts (architecture spec, "Inference interface"). Code that needs one
// part can take that part alone.
class LlmBackend : public Tokenizer, public ChatModel, public Embedder {
public:
    // Whether `text`, tokenized as an answer, is accepted by `grammar` (GBNF, root rule "root") from start to end.
    // Returns "" when it is; otherwise a short reason naming where it stops fitting (the first rejected token and the
    // text before it, or "the text ends before the grammar does"). Throws ContractError for a grammar that does not
    // parse. An empty grammar is a ContractError too: there is nothing to check against.
    virtual std::string CheckGrammar(const std::string& grammar, std::string_view text) = 0;
};

} // namespace morpheus::inference
```

In `src/inference/Llm.cpp`, delete the `SamplingFrom` definition (the whole function, from `Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)` to its closing brace, and the blank line before it). `RoleName` and the `#include "core/Errors.h"` stay.

In `src/inference/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_inference PUBLIC morpheus_core)
```

with

```cmake
# No library dependencies: Llm.cpp uses only core/Errors.h (ContractError), which is header-only.
```

In `src/llama/CMakeLists.txt`, `morpheus_llama` used `core` (`core::PathText`) through `inference`; it links it itself now. Replace

```cmake
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE llama
)
```

with

```cmake
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE morpheus_core llama
)
```

- [ ] **Step 4: Move SamplingFrom to turn**

In `src/turn/Session.h`, after the line `constexpr int REVIEW_RETRY_OFFSET = 1 << 23;`, insert:

```cpp

// The sampling for one call from its settings.json values and the call's seed (core::SamplingSeed).
inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);
```

In `src/turn/Session.cpp`, after the line `} // namespace` that closes the anonymous namespace (just before `Session::Session(`), insert:

```cpp
inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)
{
    inference::Sampling sampling;
    sampling.temperature = static_cast<float>(settings.temperature);
    sampling.topK = settings.topK;
    sampling.topP = static_cast<float>(settings.topP);
    sampling.seed = seed;
    return sampling;
}

```

Then replace each of the six `inference::SamplingFrom(` in `src/turn/Session.cpp` with `SamplingFrom(` (the file is inside `namespace morpheus::turn`). Check: `grep -n "inference::SamplingFrom" src tools tests -r` prints only `tools/probe/main.cpp`.

In `tools/probe/main.cpp`, add `#include "turn/Session.h"` after `#include "llama/LlamaCppBackend.h"`, and replace

```cpp
        spec.sampling = inference::SamplingFrom(settings.narrateSampling, 1234);
```

with

```cpp
        spec.sampling = turn::SamplingFrom(settings.narrateSampling, 1234);
```

In `tools/probe/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_llama)
```

with

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_turn morpheus_llama)
```

If the build reports `'Settings': is not a member of 'morpheus::core'` (or `SamplingSettings`, `BudgetSettings`) in a file that relied on `inference/Llm.h` including `core/Settings.h`, add `#include "core/Settings.h"` to that file. (A search found every such file already includes it through another header.)

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: builds with no warnings; `100% tests passed, 0 tests failed out of 370`.

- [ ] **Step 6: Build the dev targets**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings (the probe links `morpheus_turn`; `morpheus_llama` links `morpheus_core` itself).

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes src/inference/Llm.h src/inference/Llm.cpp src/inference/CMakeLists.txt src/llama/CMakeLists.txt src/turn/Session.h src/turn/Session.cpp tools/probe/main.cpp tools/probe/CMakeLists.txt tests/inference/LlmTests.cpp tests/turn/SamplingTests.cpp
git add src/inference/Llm.h src/inference/Llm.cpp src/inference/CMakeLists.txt src/llama/CMakeLists.txt src/turn/Session.h src/turn/Session.cpp tools/probe/main.cpp tools/probe/CMakeLists.txt tests/inference/LlmTests.cpp tests/inference/SamplingTests.cpp tests/turn/SamplingTests.cpp
git commit -m "Split LlmBackend into Tokenizer, ChatModel and Embedder; add GenerationSpec::slot; move SamplingFrom to turn" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Expected: `check_bytes` prints nothing.

---

### Task 2: grammar::Check, the test matcher promoted into morpheus_grammar

The matcher moves into the library and is hardened: matching is memoised by (node, position), repetition counts are read strictly, every read error is a `ContractError`, and instead of a yes/no it returns llama.cpp's three reason texts, pointing at the furthest position any partial match reached. Every test that used `test::GbnfMatcher` switches to it, and the header is deleted.

**Files:**
- Create: `src/grammar/Check.h`, `src/grammar/Check.cpp`
- Modify: `src/grammar/CMakeLists.txt`
- Create: `tests/grammar/CheckTests.cpp`
- Delete: `tests/grammar/GbnfMatcher.h`, `tests/grammar/GbnfMatcherTests.cpp`
- Modify: `tests/grammar/GbnfTests.cpp`, `tests/grammar/SchemaTests.cpp`, `tests/grammar/PropertyTests.cpp`, `tests/turn/ResolveGrammarTests.cpp`, `tests/llama/LlamaGrammarTests.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `std::string morpheus::grammar::Check(const std::string& grammar, std::string_view text)` in `grammar/Check.h`: `""` when the root rule matches all of `text`; otherwise `"the grammar rejects '<c>' at the start"`, `"the grammar rejects '<c>' after '<text>'"` or `"the text ends before the grammar does"`. Throws `core::ContractError` for a grammar it cannot read (no root, an undefined rule, a rule defined twice, a malformed literal, class, group, escape or repetition count, a rule that comes back to itself without consuming text).

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/CheckTests.cpp` (the first three test cases are the matcher's own tests, ported):

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "core/Errors.h"
#include "grammar/Check.h"

using namespace morpheus;

TEST_CASE("grammar::Check reads literals, classes, groups and repetitions", "[grammar-check]")
{
    const std::string gbnf = R"gbnf(root ::= "a" ("b" | "cd")? [x-z]{1,2} digits
digits ::= [0-9]*
)gbnf";
    CHECK(grammar::Check(gbnf, "ax").empty());
    CHECK(grammar::Check(gbnf, "abxy").empty());
    CHECK(grammar::Check(gbnf, "acdz123").empty());
    CHECK_FALSE(grammar::Check(gbnf, "a").empty());     // [x-z] at least once
    CHECK_FALSE(grammar::Check(gbnf, "axyz").empty());  // [x-z] at most twice
    CHECK_FALSE(grammar::Check(gbnf, "abcdx").empty()); // "b" or "cd", not both
    CHECK_FALSE(grammar::Check(gbnf, "ax1a").empty());  // the root must match the whole text
    CHECK_FALSE(grammar::Check(gbnf, "").empty());

    const std::string counts = R"gbnf(root ::= "<" [0-9]{2} ("," [0-9]){0,1} [a]+ [^a-z]{1,} ">"
)gbnf";
    CHECK(grammar::Check(counts, "<12aA>").empty());
    CHECK(grammar::Check(counts, "<12,3aaa!?>").empty());
    CHECK_FALSE(grammar::Check(counts, "<1aA>").empty());      // {2} is exactly two
    CHECK_FALSE(grammar::Check(counts, "<12,3,4aA>").empty()); // {0,1} is at most one
    CHECK_FALSE(grammar::Check(counts, "<12A>").empty());      // + needs one
    CHECK_FALSE(grammar::Check(counts, "<12aa>").empty());     // {1,} needs one
}

TEST_CASE("grammar::Check decodes escapes and UTF-8 as llama.cpp does", "[grammar-check]")
{
    // The JSON string character of the grammar builder: a code point but ", \, DEL and the controls, or one escape.
    const std::string jsonString = R"gbnf(root ::= "\"" char{0,3} "\""
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
)gbnf";
    CHECK(grammar::Check(jsonString, R"("")").empty());
    CHECK(grammar::Check(jsonString, R"("abc")").empty());
    CHECK(grammar::Check(jsonString, "\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"").empty()); // three characters of 2, 3 and 4 bytes
    CHECK(grammar::Check(jsonString, R"("\n\"\u00e9")").empty());                          // three escapes
    CHECK_FALSE(grammar::Check(jsonString, R"("abcd")").empty());                         // four characters
    CHECK_FALSE(grammar::Check(jsonString, "\"a\nb\"").empty());                           // a raw control character
    CHECK_FALSE(grammar::Check(jsonString, "\"\x7F\"").empty());                           // DEL
    CHECK_FALSE(grammar::Check(jsonString, R"("\q")").empty());                           // not a JSON escape
    CHECK_FALSE(grammar::Check(jsonString, "\"\xC3\"").empty());                           // not UTF-8

    // Literal escapes: \x, \u, \n and the quote.
    const std::string escapes = R"gbnf(root ::= "\x41\u00e9\n\"" [\x30-\x32]
)gbnf";
    CHECK(grammar::Check(escapes, "A\xC3\xA9\n\"1").empty());
    CHECK_FALSE(grammar::Check(escapes, "A\xC3\xA9\n\"3").empty());
}

TEST_CASE("grammar::Check refuses grammars it cannot read with a ContractError", "[grammar-check]")
{
    CHECK_THROWS_AS(grammar::Check("root ::= missing\n", "x"), core::ContractError);              // an undefined rule
    CHECK_THROWS_AS(grammar::Check("root ::= (\"a\"\n", "x"), core::ContractError);               // an unclosed group
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\n", "x"), core::ContractError);                  // an unclosed literal
    CHECK_THROWS_AS(grammar::Check("root ::= [a-z\n", "x"), core::ContractError);                 // an unclosed class
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\")\n", "x"), core::ContractError);               // an unmatched ')'
    CHECK_THROWS_AS(grammar::Check("other ::= \"a\"\n", "x"), core::ContractError);               // no root
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\"\nroot ::= \"b\"\n", "x"), core::ContractError); // defined twice
    CHECK_THROWS_AS(grammar::Check("root ::= \"\\q\"\n", "x"), core::ContractError);              // an unknown escape
    CHECK_THROWS_AS(grammar::Check("root \"a\"\n", "x"), core::ContractError);                     // no ' ::= '
    CHECK_THROWS_AS(grammar::Check("", "x"), core::ContractError);                                 // nothing to check against

    // A rule that comes back to itself without consuming text would loop: it is refused when matched.
    CHECK_THROWS_AS(grammar::Check("root ::= a\na ::= root | \"x\"\n", "x"), core::ContractError);
}

TEST_CASE("grammar::Check names the character where the text stops fitting", "[grammar-check]")
{
    const std::string yesNo = "root ::= \"{\\\"answer\\\":\\\"\" (\"yes\" | \"no\") \"\\\"}\"\n";
    CHECK(grammar::Check(yesNo, R"({"answer":"yes"})") == "");
    CHECK(grammar::Check(yesNo, R"({"answer":"no"})") == "");

    // The furthest position any partial match reached: the character there is the one rejected.
    CHECK(grammar::Check(yesNo, "x") == "the grammar rejects 'x' at the start");
    CHECK(grammar::Check(yesNo, R"({"answer":"maybe"})") == R"(the grammar rejects 'm' after '{"answer":"')");
    CHECK(grammar::Check(yesNo, R"({"answer":"nope"})") == R"(the grammar rejects 'p' after '{"answer":"no')");
    CHECK(grammar::Check(yesNo, R"({"answer":"yes"} and more)") == R"(the grammar rejects ' ' after '{"answer":"yes"}')");

    // Every path consumed the whole text and wanted more.
    CHECK(grammar::Check(yesNo, R"({"answer":"ye)") == "the text ends before the grammar does");
    CHECK(grammar::Check(yesNo, "") == "the text ends before the grammar does");

    // The rejected character is a whole code point, however many bytes it has.
    CHECK(grammar::Check("root ::= \"caf\" \"e\"\n", "caf\xC3\xA9") == "the grammar rejects '\xC3\xA9' after 'caf'");
}

TEST_CASE("grammar::Check quotes at most the last 60 bytes before the rejected character", "[grammar-check]")
{
    const std::string letters = "root ::= [a-z]* \"!\"\n";
    // Exactly 60 bytes: quoted whole.
    CHECK(grammar::Check(letters, std::string(60, 'a') + "?") ==
          "the grammar rejects '?' after '" + std::string(60, 'a') + "'");
    // More: the last 60, with "..." in front.
    CHECK(grammar::Check(letters, std::string(65, 'a') + "?") ==
          "the grammar rejects '?' after '..." + std::string(60, 'a') + "'");
    // A cut that would fall inside a character starts at the next whole one: 61 bytes before the '?', the first
    // two of them one character, so the cut lands on its second byte and moves past it.
    const std::string before = "\xC3\xA9" + std::string(59, 'a');
    CHECK(grammar::Check("root ::= [^?]* \"!\"\n", before + "?") ==
          "the grammar rejects '?' after '..." + std::string(59, 'a') + "'");
}

TEST_CASE("grammar::Check rejects text that is not UTF-8 at its first bad byte", "[grammar-check]")
{
    // Any code point but NUL: only a byte that is not UTF-8 stops it.
    const std::string anything = "root ::= [^\\x00]*\n";
    CHECK(grammar::Check(anything, "a\xC3\xA9z").empty());
    CHECK(grammar::Check(anything, "a\xFF" "b") == "the grammar rejects '\xFF' after 'a'");
    CHECK(grammar::Check(anything, "ab\xC3") == "the grammar rejects '\xC3' after 'ab'");
}

TEST_CASE("grammar::Check reads only well-formed repetition counts", "[grammar-check]")
{
    // The counts the builder writes: {m}, {m,} and {m,n}.
    CHECK(grammar::Check("root ::= \"a\"{2}\n", "aa").empty());
    CHECK(grammar::Check("root ::= \"a\"{0}\n", "").empty());
    CHECK(grammar::Check("root ::= \"a\"{1,}\n", "aaa").empty());
    CHECK(grammar::Check("root ::= \"a\"{1,3}\n", "aaa").empty());
    CHECK_FALSE(grammar::Check("root ::= \"a\"{1,3}\n", "aaaa").empty());

    // Anything else cannot be read: no digits, a sign, a space, a third part, a least above the most, too many digits.
    for (const std::string counts : {"{}", "{,}", "{,2}", "{a}", "{1,a}", "{-1}", "{+1}", "{ 1}", "{1 }", "{1, 2}",
                                     "{1,2,3}", "{3,2}", "{1234567890}"}) {
        INFO(counts);
        CHECK_THROWS_AS(grammar::Check("root ::= \"a\"" + counts + "\n", "a"), core::ContractError);
    }
    CHECK_THROWS_AS(grammar::Check("root ::= \"a\"{1\n", "a"), core::ContractError); // an unclosed count
}

TEST_CASE("grammar::Check stays fast on nested repetitions over a long text", "[grammar-check]")
{
    // Words of a's, each readable as any mix of "a" and "aa", with optional spaces: without memoisation every way to
    // split every word would be tried again at every repetition count of the outer loop.
    const std::string words = "root ::= (word \" \"?)*\nword ::= (\"a\" | \"aa\")+\n";
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "aaaaaaaaa "; // 2,000 characters in all.
    }
    const auto start = std::chrono::steady_clock::now();
    CHECK(grammar::Check(words, text).empty());
    CHECK(grammar::Check(words, text + "b") == "the grammar rejects 'b' after '..." + text.substr(text.size() - 60) + "'");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/Check.h': No such file or directory` in `tests/grammar/CheckTests.cpp`.

- [ ] **Step 3: Write grammar::Check**

Create `src/grammar/Check.h`:

```cpp
#pragma once

#include <string>
#include <string_view>

namespace morpheus::grammar {

// Whether `text` is accepted by `grammar` (GBNF, root rule "root") from start to end, matched by Unicode code point
// as llama.cpp's grammar sampler does. Returns "" when it is; otherwise "the grammar rejects '<c>' at the start",
// "the grammar rejects '<c>' after '<text>'" (the text cut to its last 60 bytes, with "..." in front when cut), or
// "the text ends before the grammar does". Throws ContractError for a grammar it cannot read.
//
// It reads the GBNF that GrammarBuilder and Morpheus's grammars write, one rule per line ("name ::= body"): string
// literals ("..." with the escapes \" \\ \n \r \t \[ \] \xHH \uHHHH \UHHHHHHHH), character classes ([...] and [^...],
// with ranges and the same escapes), rule references, groups, "|" and the postfixes ?, *, +, {m}, {m,} and {m,n}
// (digits only, m <= n). <c> is the code point at the furthest position any partial match reached; a byte of `text`
// that does not start a UTF-8 character is a character nothing matches. A rule that comes back to itself without
// consuming text is a ContractError when it is matched. It does not model llama.cpp's load-time refusals (left
// recursion at load, the repetition limits): the builder and FromSchema stay within them, and the [llama] tests are
// the authority on them.
std::string Check(const std::string& grammar, std::string_view text);

} // namespace morpheus::grammar
```

Create `src/grammar/Check.cpp`:

```cpp
#include "grammar/Check.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"

namespace morpheus::grammar {

namespace {

// How much of the text before a rejected character a reason quotes, in bytes (as LlamaCppBackend::CheckGrammar).
constexpr std::size_t REASON_CONTEXT_BYTES = 60;
// A repetition's most when it has no upper bound ("*", "+" and "{m,}").
constexpr std::size_t NO_LIMIT = static_cast<std::size_t>(-1);
// The most digits a repetition count may have: more could overflow, and llama.cpp refuses far smaller counts.
constexpr std::size_t MAX_COUNT_DIGITS = 9;
// Stands for a byte of the text that does not start a UTF-8 character: no literal or class matches it.
constexpr char32_t NOT_UTF8 = 0xFFFFFFFF;

// One parsed expression. A rule's body is a Choice. A Reference points at the body of the rule it names once every
// rule is read.
struct Node {
    enum class Kind { Literal, Class, Reference, Sequence, Choice, Repeat };

    Kind kind = Kind::Sequence;
    std::u32string text = {};                                // Literal: the code points to match.
    std::vector<std::pair<char32_t, char32_t>> ranges = {}; // Class: inclusive code point ranges.
    bool negated = false;                                    // Class: [^...].
    std::string rule = {};                                   // Reference: the rule's name.
    const Node* pBody = nullptr;                             // Reference: that rule's body. Not owned.
    std::vector<Node> children = {};                         // Sequence, Choice: the parts; Repeat: the one item.
    std::size_t fewest = 0;                                  // Repeat: the least repetitions.
    std::size_t most = 0;                                    // Repeat: the most repetitions, or NO_LIMIT.
};

// Throws the ContractError for a grammar Check cannot read, naming the problem and the line.
[[noreturn]] void ReadError(std::string_view problem, const std::string& line)
{
    throw core::ContractError(std::format("grammar::Check: {} in: {}", problem, line));
}

// Decodes the UTF-8 character at `pos` and moves past it; false, leaving `pos` as it was, for bytes that are not
// UTF-8.
bool DecodeUtf8(std::string_view text, std::size_t& pos, char32_t& codePoint)
{
    auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char lead = byte(pos);
    std::size_t length = 0;
    std::uint32_t value = 0;
    if (lead < 0x80) {
        length = 1;
        value = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07u;
    } else {
        return false;
    }
    if (pos + length > text.size()) {
        return false;
    }
    // Each continuation byte is 10xxxxxx and adds six bits.
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(pos + i) & 0xC0) != 0x80) {
            return false;
        }
        value = (value << 6) | (byte(pos + i) & 0x3Fu);
    }
    pos += length;
    codePoint = static_cast<char32_t>(value);
    return true;
}

// One character of a literal or a class at `pos` of `line`: an escape, or one UTF-8 code point. Moves past it.
char32_t ParseChar(const std::string& line, std::size_t& pos)
{
    if (line[pos] != '\\') {
        char32_t codePoint = 0;
        if (!DecodeUtf8(line, pos, codePoint)) {
            ReadError("invalid UTF-8", line);
        }
        return codePoint;
    }
    if (pos + 1 >= line.size()) {
        ReadError("an escape at the end", line);
    }
    const char escape = line[pos + 1];
    pos += 2;
    // The single-character escapes, or the number of hex digits that follow.
    std::size_t digits = 0;
    switch (escape) {
    case 'n':
        return U'\n';
    case 'r':
        return U'\r';
    case 't':
        return U'\t';
    case '\\':
    case '"':
    case '[':
    case ']':
        return static_cast<char32_t>(escape);
    case 'x':
        digits = 2;
        break;
    case 'u':
        digits = 4;
        break;
    case 'U':
        digits = 8;
        break;
    default:
        ReadError(std::format("an unknown escape '\\{}'", escape), line);
    }
    if (pos + digits > line.size()) {
        ReadError("a short hex escape", line);
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const char c = line[pos + i];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            ReadError("a bad hex digit", line);
        }
        value = value * 16 + digit;
    }
    pos += digits;
    return static_cast<char32_t>(value);
}

Node ParseChoice(const std::string& line, std::size_t& pos);

// A string literal, a character class, a group or a rule reference at `pos` of `line`. Recursive through groups.
Node ParsePrimary(const std::string& line, std::size_t& pos)
{
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    Node node;
    if (line[pos] == '"') {
        node.kind = Node::Kind::Literal;
        ++pos;
        while (pos < line.size() && line[pos] != '"') {
            node.text += ParseChar(line, pos);
        }
        if (pos >= line.size()) {
            ReadError("an unclosed string literal", line);
        }
        ++pos;
    } else if (line[pos] == '[') {
        node.kind = Node::Kind::Class;
        ++pos;
        if (pos < line.size() && line[pos] == '^') {
            node.negated = true;
            ++pos;
        }
        while (pos < line.size() && line[pos] != ']') {
            // One character, or a range "a-b" (a "-" just before "]" is a plain character).
            const char32_t low = ParseChar(line, pos);
            char32_t high = low;
            if (pos + 1 < line.size() && line[pos] == '-' && line[pos + 1] != ']') {
                ++pos;
                high = ParseChar(line, pos);
            }
            node.ranges.emplace_back(low, high);
        }
        if (pos >= line.size()) {
            ReadError("an unclosed character class", line);
        }
        ++pos;
    } else if (line[pos] == '(') {
        ++pos;
        node = ParseChoice(line, pos);
        if (pos >= line.size() || line[pos] != ')') {
            ReadError("an unclosed '('", line);
        }
        ++pos;
    } else if (isNameChar(line[pos])) {
        node.kind = Node::Kind::Reference;
        while (pos < line.size() && isNameChar(line[pos])) {
            node.rule += line[pos++];
        }
    } else {
        ReadError(std::format("an unexpected '{}'", line[pos]), line);
    }
    return node;
}

// Items, each with its postfixes, up to a "|", a ")" or the end of `line`. Recursive through groups.
Node ParseSequence(const std::string& line, std::size_t& pos)
{
    Node sequence;
    sequence.kind = Node::Kind::Sequence;
    for (;;) {
        while (pos < line.size() && line[pos] == ' ') {
            ++pos;
        }
        if (pos >= line.size() || line[pos] == '|' || line[pos] == ')') {
            return sequence;
        }
        Node item = ParsePrimary(line, pos);
        // Each postfix wraps the item in a repetition: ? is {0,1}, * is {0,}, + is {1,}.
        while (pos < line.size() && (line[pos] == '?' || line[pos] == '*' || line[pos] == '+' || line[pos] == '{')) {
            Node repeat;
            repeat.kind = Node::Kind::Repeat;
            const char postfix = line[pos++];
            if (postfix == '?') {
                repeat.fewest = 0;
                repeat.most = 1;
            } else if (postfix == '*') {
                repeat.fewest = 0;
                repeat.most = NO_LIMIT;
            } else if (postfix == '+') {
                repeat.fewest = 1;
                repeat.most = NO_LIMIT;
            } else {
                // {m}, {m,} or {m,n}: decimal digits only, m <= n. Anything else is not a count the builder writes.
                const std::size_t close = line.find('}', pos);
                if (close == std::string::npos) {
                    ReadError("an unclosed '{'", line);
                }
                const std::string counts = line.substr(pos, close - pos);
                pos = close + 1;
                // One count: 1 to MAX_COUNT_DIGITS decimal digits.
                auto readCount = [&](std::string_view digits) -> std::size_t {
                    const bool allDigits =
                        std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
                    if (digits.empty() || digits.size() > MAX_COUNT_DIGITS || !allDigits) {
                        ReadError(std::format("a repetition count '{{{}}}' that is not m, m, or m,n", counts), line);
                    }
                    std::size_t value = 0;
                    for (const char c : digits) {
                        value = value * 10 + static_cast<std::size_t>(c - '0');
                    }
                    return value;
                };
                const std::size_t comma = counts.find(',');
                repeat.fewest = readCount(std::string_view(counts).substr(0, comma));
                if (comma == std::string::npos) {
                    repeat.most = repeat.fewest;
                } else if (comma + 1 == counts.size()) {
                    repeat.most = NO_LIMIT;
                } else {
                    repeat.most = readCount(std::string_view(counts).substr(comma + 1));
                }
                if (repeat.most < repeat.fewest) {
                    ReadError(std::format("a repetition count '{{{}}}' whose least is above its most", counts), line);
                }
            }
            repeat.children.push_back(std::move(item));
            item = std::move(repeat);
        }
        sequence.children.push_back(std::move(item));
    }
}

// Alternatives separated by "|", up to a ")" or the end of `line`. Recursive through groups.
Node ParseChoice(const std::string& line, std::size_t& pos)
{
    Node choice;
    choice.kind = Node::Kind::Choice;
    choice.children.push_back(ParseSequence(line, pos));
    while (pos < line.size() && line[pos] == '|') {
        ++pos;
        choice.children.push_back(ParseSequence(line, pos));
    }
    return choice;
}

// Points every reference inside `node` (part of the rule `name`) at the body of the rule it names. Throws
// ContractError for a rule that does not exist. Recursive over the tree.
void Link(Node& node, const std::map<std::string, Node>& rules, const std::string& name)
{
    if (node.kind == Node::Kind::Reference) {
        const auto found = rules.find(node.rule);
        if (found == rules.end()) {
            throw core::ContractError(
                std::format("grammar::Check: '{}' used by '{}' is not defined", node.rule, name));
        }
        node.pBody = &found->second;
    }
    for (Node& child : node.children) {
        Link(child, rules, name);
    }
}

// Matches a read grammar against one text's code points by sets of end positions: every place an expression can
// end when it starts at a position. Each (node, position) is matched once and remembered, so a long text and nested
// repetitions stay polynomial. Tracks the furthest position any literal or class reached: where the text stops
// fitting when the root does not match all of it.
class Matcher {
private:
    const std::u32string* _pText = nullptr; // Not owned.
    std::map<std::pair<const Node*, std::size_t>, std::set<std::size_t>> _memo = {};
    std::set<std::pair<const Node*, std::size_t>> _active = {}; // Rule bodies being matched, with their start.
    std::size_t _furthest = 0;

public:
    explicit Matcher(const std::u32string& text)
        : _pText(&text)
    {
    }

    std::size_t Furthest() const
    {
        return _furthest;
    }

    // Where `node` can end when it starts at `pos`. Throws ContractError for a rule that comes back to itself
    // without consuming text, which would never end. Recursive over the grammar.
    std::set<std::size_t> Ends(const Node& node, std::size_t pos)
    {
        const std::u32string& text = *_pText;

        // Literals and classes match at once; they are what moves the furthest position.
        if (node.kind == Node::Kind::Literal) {
            std::size_t matched = 0;
            while (matched < node.text.size() && pos + matched < text.size() &&
                   text[pos + matched] == node.text[matched]) {
                ++matched;
            }
            _furthest = std::max(_furthest, pos + matched);
            return matched == node.text.size() ? std::set<std::size_t>{pos + matched} : std::set<std::size_t>{};
        }
        if (node.kind == Node::Kind::Class) {
            if (pos >= text.size() || text[pos] == NOT_UTF8) {
                return {};
            }
            const char32_t c = text[pos];
            const bool inRanges = std::any_of(node.ranges.begin(), node.ranges.end(),
                                              [c](const auto& range) { return range.first <= c && c <= range.second; });
            if (inRanges == node.negated) {
                return {};
            }
            _furthest = std::max(_furthest, pos + 1);
            return {pos + 1};
        }

        // Everything else is matched once per position and remembered.
        const std::pair<const Node*, std::size_t> key(&node, pos);
        if (const auto known = _memo.find(key); known != _memo.end()) {
            return known->second;
        }
        std::set<std::size_t> ends;
        switch (node.kind) {
        case Node::Kind::Reference: {
            // A rule body entered again at the same position before it finished would loop forever.
            const std::pair<const Node*, std::size_t> call(node.pBody, pos);
            if (!_active.insert(call).second) {
                throw core::ContractError(
                    std::format("grammar::Check: '{}' comes back to itself without consuming text", node.rule));
            }
            ends = Ends(*node.pBody, pos);
            _active.erase(call);
            break;
        }
        case Node::Kind::Sequence: {
            // Each part starts wherever the parts before it can end.
            ends.insert(pos);
            for (const Node& child : node.children) {
                std::set<std::size_t> next;
                for (const std::size_t start : ends) {
                    const std::set<std::size_t> more = Ends(child, start);
                    next.insert(more.begin(), more.end());
                }
                ends = std::move(next);
                if (ends.empty()) {
                    break;
                }
            }
            break;
        }
        case Node::Kind::Choice:
            for (const Node& child : node.children) {
                const std::set<std::size_t> more = Ends(child, pos);
                ends.insert(more.begin(), more.end());
            }
            break;
        case Node::Kind::Repeat: {
            // `current` holds where exactly `count` repetitions can end. With no upper bound, a position already
            // reached at a count of at least `fewest` has nothing new to offer and is not followed again. A step that
            // gives back the set it started from (the item matched only empty text) would repeat forever: its set is
            // final, and it counts for `fewest` too, which is within `most`.
            std::set<std::size_t> current = {pos};
            if (node.fewest == 0) {
                ends.insert(pos);
            }
            for (std::size_t count = 1; !current.empty() && (node.most == NO_LIMIT || count <= node.most); ++count) {
                std::set<std::size_t> next;
                for (const std::size_t start : current) {
                    const std::set<std::size_t> more = Ends(node.children[0], start);
                    next.insert(more.begin(), more.end());
                }
                const bool stuck = next == current;
                if (node.most == NO_LIMIT && count >= node.fewest) {
                    std::erase_if(next, [&ends](std::size_t end) { return ends.contains(end); });
                }
                if (count >= node.fewest || stuck) {
                    ends.insert(next.begin(), next.end());
                }
                if (stuck) {
                    break;
                }
                current = std::move(next);
            }
            break;
        }
        case Node::Kind::Literal:
        case Node::Kind::Class:
            break; // Matched above.
        }
        _memo.emplace(key, ends);
        return ends;
    }
};

} // namespace

std::string Check(const std::string& grammar, std::string_view text)
{
    // Read the rules, one per line ("name ::= body"); blank lines are skipped.
    std::map<std::string, Node> rules;
    for (std::size_t start = 0; start < grammar.size();) {
        std::size_t end = grammar.find('\n', start);
        if (end == std::string::npos) {
            end = grammar.size();
        }
        const std::string line = grammar.substr(start, end - start);
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        const std::size_t arrow = line.find(" ::= ");
        if (arrow == std::string::npos) {
            ReadError("a line without ' ::= '", line);
        }
        std::size_t pos = arrow + 5;
        Node body = ParseChoice(line, pos);
        // ParseChoice stops at a ")" it did not open, or at the end of the line.
        if (pos != line.size()) {
            ReadError("an unmatched ')'", line);
        }
        const std::string name = line.substr(0, arrow);
        if (!rules.emplace(name, std::move(body)).second) {
            ReadError(std::format("'{}' defined twice", name), line);
        }
    }
    if (!rules.contains("root")) {
        throw core::ContractError("grammar::Check: the grammar has no root rule");
    }
    // Every reference points at its rule's body; the map's nodes never move from here on.
    for (auto& [name, body] : rules) {
        Link(body, rules, name);
    }

    // The text's code points, and where each starts in bytes (plus the end), so a reason can quote the text. A byte
    // that does not start a UTF-8 character becomes NOT_UTF8, which nothing matches, and decoding goes on after it.
    std::u32string codePoints;
    std::vector<std::size_t> offsets;
    for (std::size_t pos = 0; pos < text.size();) {
        offsets.push_back(pos);
        char32_t codePoint = 0;
        if (DecodeUtf8(text, pos, codePoint)) {
            codePoints += codePoint;
        } else {
            codePoints += NOT_UTF8;
            ++pos;
        }
    }
    offsets.push_back(text.size());

    Matcher matcher(codePoints);
    if (matcher.Ends(rules.at("root"), 0).contains(codePoints.size())) {
        return {};
    }

    // Where the text stops fitting: the furthest position any partial match reached. At the end, every path wanted
    // more text.
    const std::size_t furthest = matcher.Furthest();
    if (furthest >= codePoints.size()) {
        return "the text ends before the grammar does";
    }
    const std::string_view rejected = text.substr(offsets[furthest], offsets[furthest + 1] - offsets[furthest]);
    if (furthest == 0) {
        return std::format("the grammar rejects '{}' at the start", rejected);
    }
    // The text before it: its last REASON_CONTEXT_BYTES bytes, starting on a whole UTF-8 character.
    const std::string_view before = text.substr(0, offsets[furthest]);
    std::size_t cut = before.size() > REASON_CONTEXT_BYTES ? before.size() - REASON_CONTEXT_BYTES : 0;
    while (cut < before.size() && (static_cast<unsigned char>(before[cut]) & 0xC0) == 0x80) {
        ++cut; // A continuation byte: start at the next character.
    }
    return std::format("the grammar rejects '{}' after '{}{}'", rejected, cut > 0 ? "..." : "", before.substr(cut));
}

} // namespace morpheus::grammar
```

(`ParseChar`, `ParsePrimary`, `ParseSequence` and `ParseChoice` are mutually recursive through groups, and `ParseChar` has three call sites; `Link` and `Matcher::Ends` recurse over the tree; `ReadError` and `DecodeUtf8` have several callers.)

In `src/grammar/CMakeLists.txt`, replace

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
    Schema.cpp
)
```

with

```cmake
add_library(morpheus_grammar STATIC
    Check.cpp
    Gbnf.cpp
    Schema.cpp
)
```

- [ ] **Step 4: Run the new tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar-check]"`
Expected: `All tests passed` (8 test cases).

- [ ] **Step 5: Switch every matcher user to grammar::Check and delete the matcher**

The conversion is mechanical, in each file below:
- the include `#include "grammar/GbnfMatcher.h"` becomes `#include "grammar/Check.h"` (keep the include block sorted: `grammar/Check.h` sorts before `grammar/Gbnf.h`);
- a matcher `const test::GbnfMatcher NAME(EXPRESSION);` becomes `const std::string NAME = EXPRESSION;`, keeping its name;
- `NAME.Accepts(TEXT)` becomes `grammar::Check(NAME, TEXT).empty()` (inside `CHECK(...)` and `CHECK_FALSE(...)` alike, so `CHECK_FALSE(NAME.Accepts(TEXT))` becomes `CHECK_FALSE(grammar::Check(NAME, TEXT).empty())`).

Files and their matchers:
- `tests/grammar/GbnfTests.cpp`: the helper becomes

  ```cpp
  // The grammar whose root is `rootBody`.
  std::string GrammarFor(const std::string& rootBody)
  {
      return grammar::GrammarBuilder().Text(rootBody);
  }
  ```

  and each `const test::GbnfMatcher NAME = MatcherFor(EXPRESSION);` becomes `const std::string NAME = GrammarFor(EXPRESSION);` (the matchers `text`, `mood`, `list`, `object`, `either`, `matcher` in the brute-force test, `all`, `top`, `bottom`, `lowest`).
- `tests/grammar/SchemaTests.cpp`: the two `matcher`s.
- `tests/grammar/PropertyTests.cpp`: `CheckRandomValues` becomes

  ```cpp
  void CheckRandomValues(const nlohmann::json& schema)
  {
      const std::string gbnf = grammar::FromSchema(schema);
      std::mt19937_64 random(test::PROPERTY_SEED);
      for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
          const std::string text = test::RandomValue(schema, random).dump();
          INFO("value " << i << ": " << text);
          const std::vector<grammar::ValidationError> errors = grammar::Validate(nlohmann::json::parse(text), schema);
          for (const grammar::ValidationError& error : errors) {
              UNSCOPED_INFO(error.path << ": " << error.message);
          }
          CHECK(errors.empty());
          CHECK(grammar::Check(gbnf, text) == "");
      }
  }
  ```

- `tests/turn/ResolveGrammarTests.cpp`: the matchers `engine` (twice), `assisted`, `lookup`, `matcher` (five times), `both`, `check`, `digest`, `creates` (twice). The comment above the characterization tests ("checked with the test matcher") becomes "checked with grammar::Check". The test cases whose locals are named `grammar` (lines 190-350) use no matcher and need no change.

Delete the matcher and its tests (their cases are now `CheckTests.cpp`'s first three):

```bash
git rm -q tests/grammar/GbnfMatcher.h tests/grammar/GbnfMatcherTests.cpp
```

Then in `tests/llama/LlamaGrammarTests.cpp` (dev build only), make the model-backed tests compare `grammar::Check` with llama.cpp. Replace `#include "grammar/GbnfMatcher.h"` with `#include "grammar/Check.h"`, and after `using Catch::Matchers::ContainsSubstring;` add:

```cpp

namespace {

// The kind of a grammar check's verdict: "fits", "rejects" or "ends early". llama.cpp's reason quotes a token and
// grammar::Check's a code point, so the two agree on the kind, not the quote.
std::string VerdictOf(const std::string& reason)
{
    if (reason.empty()) {
        return "fits";
    }
    if (reason == "the text ends before the grammar does") {
        return "ends early";
    }
    REQUIRE(reason.starts_with("the grammar rejects '"));
    return "rejects";
}

} // namespace
```

Replace the test case "llama.cpp accepts the random values of each test schema against the schema's grammar" with:

```cpp
TEST_CASE("llama.cpp and grammar::Check accept the random values of each test schema", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<nlohmann::json> schemas = {test::CatalogSchema(), test::EveryKeywordSchema()};
    for (const nlohmann::json& schema : schemas) {
        const std::string gbnf = grammar::FromSchema(schema);
        // The grammar parses (one that does not is a ContractError) and wants a whole value, by both checkers.
        CHECK(backend.CheckGrammar(gbnf, "") == "the text ends before the grammar does");
        CHECK(grammar::Check(gbnf, "") == "the text ends before the grammar does");
        // The same values as the fast suite's property test: the same seed, the same draws, written compact.
        std::mt19937_64 random(test::PROPERTY_SEED);
        for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
            const std::string text = test::RandomValue(schema, random).dump();
            INFO("value " << i << ": " << text);
            CHECK(backend.CheckGrammar(gbnf, text) == "");
            CHECK(grammar::Check(gbnf, text) == "");
        }
    }
}
```

In the test case "llama.cpp rejects invalid actions against the catalog grammar", replace its loop with:

```cpp
    for (const std::string& text : invalid) {
        INFO(text);
        const std::string reason = backend.CheckGrammar(gbnf, text);
        CHECK(reason != "");
        CHECK(VerdictOf(grammar::Check(gbnf, text)) == VerdictOf(reason));
    }
```

Replace the test case "llama.cpp and the test matcher agree on IntRange grammars" with:

```cpp
TEST_CASE("llama.cpp and grammar::Check agree on IntRange grammars", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<std::pair<long long, long long>> ranges = {{-15, 123}, {0, 255}, {-1000, -91}, {37, 120}};
    for (const auto& [low, high] : ranges) {
        const std::string gbnf = grammar::GrammarBuilder().Text(grammar::IntRange(low, high));
        // Other spellings, then a window around each end.
        std::vector<std::pair<std::string, bool>> probes = {{"-0", false}, {"00", false}, {"007", false}, {"-01", false},
                                                            {"+5", false}};
        for (long long value = low - 12; value <= low + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (long long value = high - 12; value <= high + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (const auto& [text, inRange] : probes) {
            INFO("IntRange(" << low << ", " << high << ") on '" << text << "'");
            const std::string llamaReason = backend.CheckGrammar(gbnf, text);
            const std::string checkReason = grammar::Check(gbnf, text);
            CHECK(llamaReason.empty() == inRange);
            CHECK(checkReason.empty() == inRange);
            CHECK(VerdictOf(checkReason) == VerdictOf(llamaReason));
        }
    }
}
```

Check that nothing refers to the matcher any more: `grep -rn "GbnfMatcher\|\.Accepts(" tests src` prints nothing.

- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: builds with no warnings; `100% tests passed, 0 tests failed out of 375` (370, minus the matcher's 3, plus `CheckTests.cpp`'s 8).

- [ ] **Step 7: Build the dev targets and run the model-backed grammar tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run (Git Bash, from the repo root):

```bash
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*" "LlamaCppBackend::CheckGrammar*"
```

Expected: `All tests passed`, none skipped. A verdict-kind mismatch in the IntRange test is a real disagreement between `grammar::Check` and llama.cpp: report the probe and both reasons rather than loosening the check.

- [ ] **Step 8: Byte check and commit**

```bash
check_bytes src/grammar/Check.h src/grammar/Check.cpp src/grammar/CMakeLists.txt tests/grammar/CheckTests.cpp tests/grammar/GbnfTests.cpp tests/grammar/SchemaTests.cpp tests/grammar/PropertyTests.cpp tests/turn/ResolveGrammarTests.cpp tests/llama/LlamaGrammarTests.cpp
git add src/grammar/Check.h src/grammar/Check.cpp src/grammar/CMakeLists.txt tests/grammar/CheckTests.cpp tests/grammar/GbnfMatcher.h tests/grammar/GbnfMatcherTests.cpp tests/grammar/GbnfTests.cpp tests/grammar/SchemaTests.cpp tests/grammar/PropertyTests.cpp tests/turn/ResolveGrammarTests.cpp tests/llama/LlamaGrammarTests.cpp
git commit -m "Promote the GBNF matcher into morpheus_grammar as grammar::Check" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Expected: `check_bytes` prints nothing (the `\x` and `\u` escapes in `CheckTests.cpp` must still be escapes).

---

### Task 3: CheckGrammar leaves the interface; the model seat checks with grammar::Check

`LlmBackend` becomes exactly the union of its three parts. The model seat's Check calls `grammar::Check`; `LlamaCppBackend` keeps its llama.cpp-based checker as a plain method for the `[llama]` agreement tests; every other implementation drops `CheckGrammar`, and `ScriptedBackend` loses its grammar-verdict hooks.

**Files:**
- Modify: `src/inference/Llm.h`
- Modify: `src/llama/LlamaCppBackend.h`
- Modify: `src/turn/Replay.h`, `src/turn/Replay.cpp`
- Modify: `src/app/model/ModelSeat.h`, `src/app/model/ModelSeat.cpp`, `src/app/model/ModelSeatBackend.h`, `src/app/model/ModelSeatBackend.cpp`, `src/app/CMakeLists.txt`
- Modify: `tests/fakes/FakeBackend.h`, `tests/fakes/ScriptedBackend.h`
- Test: `tests/app/ModelSeatTests.cpp`, `tests/app/ModelSeatBackendTests.cpp`, `tests/app/UiTests.cpp`, `tests/turn/ReplayTests.cpp`

**Interfaces:**
- Consumes: `grammar::Check` (Task 2); the split of Task 1.
- Produces: `class inference::LlmBackend : public Tokenizer, public ChatModel, public Embedder {};` (no members of its own). `std::string llama::LlamaCppBackend::CheckGrammar(const std::string& grammar, std::string_view text)`, no longer an override. `app::ModelSeat::Check` fills `SeatCheck::problem` from `grammar::Check`. `test::ScriptedBackend` has no `SetGrammarVerdict`, `GrammarChecks` or `GrammarCheck`.

- [ ] **Step 1: Write the failing test**

In `tests/app/ModelSeatTests.cpp`, test case "ModelSeat publishes a request, checks a reply against its grammar, and answers it with Submit": delete the line

```cpp
    backend.SetGrammarVerdict("the grammar rejects 'maybe' at the start");
```

and replace the block

```cpp
    // Check asks the backend that published the request, with the request's grammar.
    const std::optional<app::SeatCheck> checked = seat.Check(published.id, "maybe");
    REQUIRE(checked);
    CHECK(checked->problem == "the grammar rejects 'maybe' at the start");
    CHECK(checked->tokens == backend.CountTokens("maybe"));
    REQUIRE(backend.GrammarChecks().size() == 1);
    CHECK(backend.GrammarChecks()[0].grammar == request.spec.grammar);
    CHECK(backend.GrammarChecks()[0].text == "maybe");
```

with

```cpp
    // Check fits the reply to the request's grammar with grammar::Check, and counts its tokens with the backend that
    // published the request.
    const std::optional<app::SeatCheck> checked = seat.Check(published.id, "maybe");
    REQUIRE(checked);
    CHECK(checked->problem == "the grammar rejects 'm' at the start");
    CHECK(checked->tokens == backend.CountTokens("maybe"));
    CHECK(seat.Check(published.id, "yes")->problem.empty());
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[model_seat]"`
Expected: FAIL in "ModelSeat publishes a request, ...": `CHECK( checked->problem == "the grammar rejects 'm' at the start" )` with expansion `"" == "the grammar rejects 'm' at the start"` (the scripted backend's default verdict).

- [ ] **Step 3: Check with grammar::Check and take CheckGrammar off the interface**

In `src/inference/Llm.h`, replace

```cpp
// The engine's view of its models: all three parts (architecture spec, "Inference interface"). Code that needs one
// part can take that part alone.
class LlmBackend : public Tokenizer, public ChatModel, public Embedder {
public:
    // Whether `text`, tokenized as an answer, is accepted by `grammar` (GBNF, root rule "root") from start to end.
    // Returns "" when it is; otherwise a short reason naming where it stops fitting (the first rejected token and the
    // text before it, or "the text ends before the grammar does"). Throws ContractError for a grammar that does not
    // parse. An empty grammar is a ContractError too: there is nothing to check against.
    virtual std::string CheckGrammar(const std::string& grammar, std::string_view text) = 0;
};
```

with

```cpp
// The engine's view of its models: all three parts (architecture spec, "Inference interface"). Code that needs one
// part can take that part alone. Whether a text fits a grammar is grammar::Check's job, independent of any backend.
class LlmBackend : public Tokenizer, public ChatModel, public Embedder {};
```

In `src/llama/LlamaCppBackend.h`, replace

```cpp
    // The grammar sampler alone over the text's tokens: no decode, so the KV cache is untouched.
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
```

with

```cpp
    // Whether `text`, tokenized as an answer, is accepted by `grammar` (GBNF, root rule "root") from start to end, by
    // llama.cpp's own grammar sampler over the text's tokens: no decode, so the KV cache is untouched. Returns "" when
    // it is; otherwise a short reason naming the first rejected token and the text before it, or "the text ends
    // before the grammar does". Throws ContractError for an empty grammar or one that does not parse. Not part of
    // LlmBackend: the model-backed tests use it to confirm that grammar::Check agrees with llama.cpp.
    std::string CheckGrammar(const std::string& grammar, std::string_view text);
```

(`LlamaCppBackend.cpp`'s `CheckGrammar` body does not change.)

In `src/turn/Replay.h`, delete the line

```cpp
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
```

and in `src/turn/Replay.cpp` delete the definition

```cpp
std::string RecordingBackend::CheckGrammar(const std::string& grammar, std::string_view text)
{
    return _pInner->CheckGrammar(grammar, text); // A check is not a completion: nothing is recorded.
}

```

In `src/app/model/ModelSeatBackend.h`, delete the line

```cpp
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
```

and in `src/app/model/ModelSeatBackend.cpp` delete the definition

```cpp
std::string ModelSeatBackend::CheckGrammar(const std::string& grammar, std::string_view text)
{
    return _pInner->CheckGrammar(grammar, text);
}

```

In `src/app/model/ModelSeat.h`, replace

```cpp
    std::string problem = {}; // LlmBackend::CheckGrammar's verdict: "" when the reply fits the request's grammar.
```

with

```cpp
    std::string problem = {}; // grammar::Check's verdict: "" when the reply fits the request's grammar.
```

and replace the comment of `Check`

```cpp
    // Checks `text` against request `id`'s grammar with the backend that published it, and counts its tokens.
```

with

```cpp
    // Checks `text` against request `id`'s grammar (grammar::Check), and counts its tokens with the backend that
    // published the request.
```

In `src/app/model/ModelSeat.cpp`, add `#include "grammar/Check.h"` after `#include "core/Errors.h"`, and in `ModelSeat::Check` replace

```cpp
    // The worker is blocked in Await and cannot return from it while this lock is held, so the backend is idle.
    SeatCheck check;
    check.problem = _pBackend->CheckGrammar(_pending->spec.grammar, text);
    check.tokens = _pBackend->CountTokens(text);
```

with

```cpp
    // The worker is blocked in Await and cannot return from it while this lock is held, so the backend is idle.
    SeatCheck check;
    check.problem = grammar::Check(_pending->spec.grammar, text);
    check.tokens = _pBackend->CountTokens(text);
```

In `src/app/CMakeLists.txt`, the model library now calls `grammar` directly; replace

```cmake
target_link_libraries(morpheus_app_model PUBLIC morpheus_turn)
```

with

```cmake
target_link_libraries(morpheus_app_model PUBLIC morpheus_turn morpheus_grammar)
```

In `tests/fakes/FakeBackend.h`, replace the class comment's second sentence

```cpp
// message. Generation, the grammar check, embeddings and state are not used by those tests and throw if called.
```

with

```cpp
// message. Generation, embeddings and state are not used by those tests and throw if called.
```

and delete

```cpp
    std::string CheckGrammar(const std::string&, std::string_view) override
    {
        throw core::ContractError("FakeBackend::CheckGrammar is not available");
    }

```

In `tests/fakes/ScriptedBackend.h`, delete:
- the class comment's last line `// CheckGrammar records each check and answers the verdict SetGrammarVerdict set: "" (the text fits) by default.`;
- the nested struct `GrammarCheck` with its comment (`// One CheckGrammar call's arguments.` and the four lines of the struct);
- the members `std::string _grammarVerdict = {}; // What CheckGrammar answers.` and `std::vector<GrammarCheck> _grammarChecks = {};`;
- the methods `SetGrammarVerdict` and `GrammarChecks` with their comments;
- the override `std::string CheckGrammar(const std::string& grammar, std::string_view text) override { ... }` (five lines with its braces).

- [ ] **Step 4: Update the other tests that used the verdict hooks**

In `tests/app/ModelSeatTests.cpp`, test case "ModelSeat::Check needs a request with a grammar": delete the line `CHECK(backend.GrammarChecks().empty());`.

In `tests/app/ModelSeatBackendTests.cpp`, delete the five lines

```cpp
    scripted.SetGrammarVerdict("the grammar rejects 'no' at the start");
    CHECK(backend.CheckGrammar(R"(root ::= "yes")", "no") == "the grammar rejects 'no' at the start");
    REQUIRE(scripted.GrammarChecks().size() == 1);
    CHECK(scripted.GrammarChecks()[0].grammar == R"(root ::= "yes")");
    CHECK(scripted.GrammarChecks()[0].text == "no");
```

In `tests/turn/ReplayTests.cpp`, delete the whole test case "RecordingBackend forwards the grammar check and records nothing for it" (from its `TEST_CASE` line to its closing brace, and the blank line after it).

In `tests/app/UiTests.cpp`, test case "The Model seat window shows a Draft call's draft, checks it and submits it": replace

```cpp
    // The factory hands out the ScriptedBackend it made, to see the grammar checks reach it.
    std::atomic<test::ScriptedBackend*> pMade = nullptr;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder());
    app::AppController controller(test::PathsIn(dir), [&](const core::Settings& settings) {
        std::unique_ptr<inference::LlmBackend> pBackend = scripted(settings);
        pMade = static_cast<test::ScriptedBackend*>(pBackend.get());
        return pBackend;
    });
```

with

```cpp
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
```

and replace

```cpp
    // Check: the reply's token count and the grammar's verdict, from the real backend. It answers nothing.
    PressSeatItem("Check");
    DrawSeatWindow(imgui, controller, window, 2);
    REQUIRE(pMade.load() != nullptr);
    REQUIRE(pMade.load()->GrammarChecks().size() == 1);
    CHECK(pMade.load()->GrammarChecks()[0].text == MOVE);
    CHECK(controller.Seat().Pending());
```

with

```cpp
    // Check: the reply's token count by the real backend and grammar::Check's verdict. It answers nothing.
    PressSeatItem("Check");
    DrawSeatWindow(imgui, controller, window, 2);
    CHECK(controller.Seat().Pending());
    const std::optional<app::SeatCheck> checked = controller.Seat().Check(request.id, MOVE);
    REQUIRE(checked);
    CHECK(checked->problem == grammar::Check(request.spec.grammar, MOVE));
    CHECK(checked->tokens == static_cast<int>((MOVE.size() + 3) / 4)); // The scripted tokenizer: 4 bytes a token.
```

Add `#include "grammar/Check.h"` to `UiTests.cpp`'s project includes (after `#include "core/MemoryMode.h"`). If `<atomic>` is no longer used in `UiTests.cpp` (`grep -n "atomic" tests/app/UiTests.cpp` prints only the include), delete `#include <atomic>`.

Check: `grep -rn "CheckGrammar" src tests tools` prints only `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp` and `tests/llama/LlamaGrammarTests.cpp`; `grep -rn "GrammarVerdict\|GrammarChecks" tests` prints nothing.

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: builds with no warnings; `100% tests passed, 0 tests failed out of 374` (one test case deleted from `ReplayTests.cpp`).

- [ ] **Step 6: Build the dev targets and run the model-backed grammar tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run, with the two model variables exported as in Task 2: `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*" "LlamaCppBackend::CheckGrammar*"`
Expected: `All tests passed`.

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes src/inference/Llm.h src/llama/LlamaCppBackend.h src/turn/Replay.h src/turn/Replay.cpp src/app/model/ModelSeat.h src/app/model/ModelSeat.cpp src/app/model/ModelSeatBackend.h src/app/model/ModelSeatBackend.cpp src/app/CMakeLists.txt tests/fakes/FakeBackend.h tests/fakes/ScriptedBackend.h tests/app/ModelSeatTests.cpp tests/app/ModelSeatBackendTests.cpp tests/app/UiTests.cpp tests/turn/ReplayTests.cpp
git add src/inference/Llm.h src/llama/LlamaCppBackend.h src/turn/Replay.h src/turn/Replay.cpp src/app/model/ModelSeat.h src/app/model/ModelSeat.cpp src/app/model/ModelSeatBackend.h src/app/model/ModelSeatBackend.cpp src/app/CMakeLists.txt tests/fakes/FakeBackend.h tests/fakes/ScriptedBackend.h tests/app/ModelSeatTests.cpp tests/app/ModelSeatBackendTests.cpp tests/app/UiTests.cpp tests/turn/ReplayTests.cpp
git commit -m "Take CheckGrammar off LlmBackend: the model seat checks with grammar::Check" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: cpp-httplib and the fake llama-server

The new dependency, its option, and the test double every later task uses. `tests/fakes/FakeLlamaServer.h` is a cpp-httplib server on a free local port that answers llama-server's endpoints from a script, streams completions as llama-server does, and records every request.

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/fakes/FakeLlamaServer.h`
- Test: `tests/llamaserver/FakeLlamaServerTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: CMake option `MORPHEUS_BUILD_LLAMA_SERVER` (default ON); INTERFACE target `morpheus_httplib` (SYSTEM include of cpp-httplib's folder, links `Threads::Threads` and on Windows `ws2_32`). `class morpheus::test::FakeLlamaServer` with:
  - `struct Request { std::string method; std::string path; nlohmann::json body; }` (`body` null for a GET);
  - `struct CompletionScript { std::vector<std::string> pieces; std::string stopType = "eos"; int promptN = -1; int tokensEvaluated = -1; double promptMs = 12.5; double predictedMs = 40.0; int predictedN = -1; std::string extraLine; bool endEarly = false; bool holdOpen = false; }`;
  - `using Tokenizer = std::function<std::vector<int>(const std::string& content, bool addSpecial, bool parseSpecial)>`, `using Renderer = std::function<std::string(const nlohmann::json& body)>`, `using EmbeddingMaker = std::function<std::vector<std::vector<float>>(const std::string& content)>`;
  - `std::string Url() const` (`"http://127.0.0.1:<port>"`), `static std::vector<int> DefaultTokens(const std::string& content, bool addSpecial, bool parseSpecial)` (the default tokenizer), `void SetProps(nlohmann::json)`, `void SetTokenizer(Tokenizer)`, `void SetRenderer(Renderer)`, `void SetEmbeddingMaker(EmbeddingMaker)`, `void QueueCompletion(CompletionScript)`, `void SetStatus(const std::string& path, int status)` (0 clears), `std::vector<Request> Requests() const`, `std::vector<Request> RequestsTo(const std::string& path) const`, `bool ClientClosedStream() const`.
  - Defaults: `/props` = `{"default_generation_settings": {"n_ctx": 16384}, "total_slots": 3, "chat_template": "<a chatml template without enable_thinking>", "model_path": "/models/nomic-embed-text-v1.5.Q8_0.gguf"}`; `DefaultTokens`: one token per 4 bytes (the last one shorter), whatever the flags, a token's id its first byte plus 256 times its length, no BOS; a `/completion` prompt is token ids (its length is `tokens_evaluated`) or text (tokenized with `add_special` and `parse_special`); chatml rendering (`<|im_start|>role\ncontent<|im_end|>\n` per message, then `<|im_start|>assistant\n` when `add_generation_prompt`); the embedding `{{3, 4}}`.

- [ ] **Step 1: Write the failing tests**

Create `tests/llamaserver/FakeLlamaServerTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "fakes/FakeLlamaServer.h"

using namespace morpheus;

TEST_CASE("FakeLlamaServer answers from its script and records each request", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    httplib::Client client(fake.Url());

    const httplib::Result props = client.Get("/props");
    REQUIRE(props);
    CHECK(props->status == 200);
    CHECK(nlohmann::json::parse(props->body).at("default_generation_settings").at("n_ctx") == 16384);

    // One token per 4 bytes, rounded up.
    const httplib::Result tokens =
        client.Post("/tokenize", R"({"content":"abcde","add_special":false,"parse_special":false})", "application/json");
    REQUIRE(tokens);
    CHECK(nlohmann::json::parse(tokens->body).at("tokens").size() == 2);

    const httplib::Result rendered = client.Post(
        "/apply-template", R"({"messages":[{"role":"user","content":"Hi"}],"add_generation_prompt":true})", "application/json");
    REQUIRE(rendered);
    CHECK(nlohmann::json::parse(rendered->body).at("prompt") == "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n");

    // llama-server's own /embedding answer: one result per text, holding the pooled vector.
    const httplib::Result embedded = client.Post("/embedding", R"({"content":"a lighthouse"})", "application/json");
    REQUIRE(embedded);
    CHECK(nlohmann::json::parse(embedded->body) == nlohmann::json::parse(R"([{"index":0,"embedding":[[3.0,4.0]]}])"));

    // A scripted failure.
    fake.SetStatus("/props", 503);
    const httplib::Result refused = client.Get("/props");
    REQUIRE(refused);
    CHECK(refused->status == 503);

    const std::vector<test::FakeLlamaServer::Request> requests = fake.Requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].path == "/props");
    CHECK(requests[0].body.is_null());
    CHECK(requests[1].method == "POST");
    CHECK(requests[1].path == "/tokenize");
    CHECK(requests[1].body.at("content") == "abcde");
    CHECK(fake.RequestsTo("/props").size() == 2);
}

TEST_CASE("FakeLlamaServer streams a scripted completion as llama-server does", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"Hel", "lo"};
    script.stopType = "limit";
    script.promptN = 1;
    fake.QueueCompletion(script);
    test::FakeLlamaServer::CompletionScript cut;
    cut.pieces = {"Hi"};
    cut.endEarly = true;
    fake.QueueCompletion(cut);

    // The messages of one streamed answer, in order: each "data: {...}" line, without blank lines and pings.
    httplib::Client client(fake.Url());
    auto stream = [&client](const std::string& body) {
        std::string received;
        const httplib::Result result =
            client.Post("/completion", httplib::Headers{}, body, "application/json", [&received](const char* pData, std::size_t length) {
                received.append(pData, length);
                return true;
            });
        REQUIRE(result);
        REQUIRE(result->status == 200);
        std::vector<nlohmann::json> messages;
        std::size_t start = 0;
        for (std::size_t end = received.find('\n'); end != std::string::npos; end = received.find('\n', start)) {
            const std::string line = received.substr(start, end - start);
            start = end + 1;
            if (line.starts_with("data: ")) {
                messages.push_back(nlohmann::json::parse(line.substr(6)));
            }
        }
        return messages;
    };

    // "abcdefgh" is 2 tokens by the default tokenizer: every message carries it as tokens_evaluated.
    const std::vector<nlohmann::json> messages = stream(R"({"prompt":"abcdefgh","id_slot":2})");
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].at("content") == "Hel");
    CHECK(messages[0].at("stop") == false);
    CHECK(messages[0].at("tokens_evaluated") == 2);
    CHECK(messages[0].at("id_slot") == 2);
    CHECK(messages[1].at("content") == "lo");
    CHECK(messages[2].at("content") == "");
    CHECK(messages[2].at("stop") == true);
    CHECK(messages[2].at("stop_type") == "limit");
    CHECK(messages[2].at("tokens_evaluated") == 2);
    CHECK(messages[2].at("tokens_cached") == 4);
    CHECK(messages[2].at("timings").at("prompt_n") == 1);
    CHECK(messages[2].at("timings").at("predicted_n") == 2);
    CHECK(messages[2].at("timings").at("prompt_ms") == 12.5);
    CHECK(messages[2].at("timings").at("predicted_ms") == 40.0);

    // A script that ends early sends its pieces and no last message. A prompt of token ids counts as its length.
    const std::vector<nlohmann::json> cutMessages = stream(R"({"prompt":[5,6,7]})");
    REQUIRE(cutMessages.size() == 1);
    CHECK(cutMessages[0].at("content") == "Hi");
    CHECK(cutMessages[0].at("tokens_evaluated") == 3);

    // With nothing left to answer, the fake fails the request as llama-server fails one.
    const httplib::Result none = client.Post("/completion", R"({"prompt":"abc"})", "application/json");
    REQUIRE(none);
    CHECK(none->status == 500);
}

TEST_CASE("FakeLlamaServer sees a client close a held stream", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"Hel"};
    script.holdOpen = true;
    fake.QueueCompletion(script);
    {
        // The receiver refuses the first bytes, which ends the request; the client closes its connection.
        httplib::Client client(fake.Url());
        const httplib::Result result = client.Post("/completion", httplib::Headers{}, R"({"prompt":"abc"})",
                                                   "application/json", [](const char*, std::size_t) { return false; });
        CHECK_FALSE(result);
        CHECK(result.error() == httplib::Error::Canceled);
    }
    // The fake notices on one of its next pings.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fake.ClientClosedStream() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake.ClientClosedStream());
}
```

In `tests/CMakeLists.txt`, after the block

```cmake
if(NOT MORPHEUS_BUILD_LLAMA)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/llama/")
endif()
```

add

```cmake
# The llama-server backend's tests need cpp-httplib; presets without it skip their sources too.
if(NOT MORPHEUS_BUILD_LLAMA_SERVER)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/llamaserver/")
endif()
```

and after the block

```cmake
if(MORPHEUS_BUILD_LLAMA)
    target_link_libraries(morpheus_tests PRIVATE morpheus_llama)
endif()
```

add

```cmake
if(MORPHEUS_BUILD_LLAMA_SERVER)
    target_link_libraries(morpheus_tests PRIVATE morpheus_httplib) # The fake llama-server.
endif()
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'fakes/FakeLlamaServer.h': No such file or directory` in `tests/llamaserver/FakeLlamaServerTests.cpp` (CMake takes the unknown `morpheus_httplib` for a library file name, so nothing fails before the compiler).

- [ ] **Step 3: Fetch cpp-httplib and add the option**

In the root `CMakeLists.txt`, after the line

```cmake
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend, morpheus_probe and morpheus_play (fetches and builds llama.cpp)" ${PROJECT_IS_TOP_LEVEL})
```

add

```cmake
# On in every build, vendored ones included: the llama-server backend needs no GPU or model, only cpp-httplib.
option(MORPHEUS_BUILD_LLAMA_SERVER "Build the llama-server backend, morpheus_llamaserver (fetches cpp-httplib, header-only, no OpenSSL)" ON)
```

After the block

```cmake
if(MORPHEUS_BUILD_APP AND NOT MORPHEUS_BUILD_LLAMA)
    message(FATAL_ERROR "MORPHEUS_BUILD_APP needs MORPHEUS_BUILD_LLAMA.")
endif()
```

add

```cmake
# The app, morpheus_play and morpheus_probe build either backend, as settings.json chooses.
if(MORPHEUS_BUILD_LLAMA AND NOT MORPHEUS_BUILD_LLAMA_SERVER)
    message(FATAL_ERROR "MORPHEUS_BUILD_LLAMA (the app, morpheus_play and morpheus_probe) needs MORPHEUS_BUILD_LLAMA_SERVER.")
endif()
```

After the SQLite block (the line `set_target_properties(morpheus_sqlite3 PROPERTIES COMPILE_WARNING_AS_ERROR OFF)`), add

```cmake

if(MORPHEUS_BUILD_LLAMA_SERVER)
    # cpp-httplib (the llama-server backend and the tests' fake server): header-only, used without OpenSSL, zlib,
    # brotli or zstd. Its own CMakeLists.txt would look for them, so it is not used: SOURCE_SUBDIR names a folder
    # without one, and FetchContent only downloads. The target is defined here under a name of Morpheus's own, like
    # SQLite's, so it never meets llama.cpp's vendored cpp-httplib target or a host project's httplib target.
    FetchContent_Declare(
        morpheus_cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        cf3693cb5cc0d39b0e6f4122ba89bda9edb5ac6b       # commit SHA to get, v0.59.0
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SOURCE_SUBDIR  no-cmake-build                                 # No CMakeLists.txt there: download only
    )
    FetchContent_MakeAvailable(morpheus_cpp_httplib)
    add_library(morpheus_httplib INTERFACE)
    # SYSTEM: its warnings are not Morpheus's (no /W4 /WX on httplib.h).
    target_include_directories(morpheus_httplib SYSTEM INTERFACE ${morpheus_cpp_httplib_SOURCE_DIR})
    find_package(Threads REQUIRED)
    target_link_libraries(morpheus_httplib INTERFACE Threads::Threads)
    if(WIN32)
        target_link_libraries(morpheus_httplib INTERFACE ws2_32)
    endif()
    if(MSVC)
        # httplib.h is about 23,000 lines of inline code: a file that includes it can pass MSVC's section limit.
        target_compile_options(morpheus_httplib INTERFACE /bigobj)
    endif()
endif()
```

- [ ] **Step 4: Write the fake server**

Create `tests/fakes/FakeLlamaServer.h`:

```cpp
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

namespace morpheus::test {

// A stand-in for llama-server in the fast suite: a cpp-httplib server on a free local port that answers the
// endpoints LlamaServerBackend uses from a script, and records every request.
// - GET /props: the props SetProps gave (by default a chatml template without a thinking switch, 16384 tokens per
//   slot, 3 slots and an embedding model's file).
// - POST /tokenize: the tokenizer's ids for content, add_special (default false) and parse_special (default true), as
//   llama-server defaults them. By default DefaultTokens.
// - POST /apply-template: the renderer's text as "prompt" (by default chatml, with the assistant's opening when
//   add_generation_prompt is set).
// - POST /completion: the next queued CompletionScript, streamed as llama-server streams: a "data: {...}" message
//   per piece, then the last message with stop_type, tokens_cached and timings. With none queued: HTTP 500.
// - POST /embedding: llama-server's own answer, [{"index": 0, "embedding": [vectors]}], with the embedding maker's
//   vectors (by default the one vector {3, 4}).
// SetStatus makes a path answer an HTTP error instead. Handlers run on the server's threads; every member they use
// is guarded by one mutex.
class FakeLlamaServer {
public:
    // One request as it arrived.
    struct Request {
        std::string method = {};
        std::string path = {};
        nlohmann::json body = nullptr; // The JSON body; null for a GET.
    };

    // One /completion answer.
    struct CompletionScript {
        std::vector<std::string> pieces = {}; // Streamed one message each, in order.
        std::string stopType = "eos";         // The last message's stop_type: "eos", "limit" or "word".
        int promptN = -1;          // timings.prompt_n, the prompt tokens evaluated; -1: all of them.
        int tokensEvaluated = -1;  // tokens_evaluated, the prompt's tokens; -1: the length of a prompt of token ids, or
                                   // a text prompt tokenized with add_special and parse_special, as llama-server does.
        double promptMs = 12.5;    // timings.prompt_ms
        double predictedMs = 40.0; // timings.predicted_ms
        int predictedN = -1;       // timings.predicted_n; -1: one per piece.
        std::string extraLine = {}; // Sent as a line of its own after the pieces, when not empty (a malformed stream).
        bool endEarly = false;      // End the stream after the pieces, without the last message.
        bool holdOpen = false;      // After the pieces, keep the stream open (pinging) until the client closes it.
    };

    using Tokenizer = std::function<std::vector<int>(const std::string& content, bool addSpecial, bool parseSpecial)>;
    using Renderer = std::function<std::string(const nlohmann::json& body)>;
    using EmbeddingMaker = std::function<std::vector<std::vector<float>>(const std::string& content)>;

private:
    mutable std::mutex _mutex = {};
    nlohmann::json _props = {{"default_generation_settings", {{"n_ctx", 16384}}},
                             {"total_slots", 3},
                             {"chat_template", "{% for message in messages %}<|im_start|>{{ message.role }}{% endfor %}"},
                             {"model_path", "/models/nomic-embed-text-v1.5.Q8_0.gguf"}};
    Tokenizer _tokenizer = DefaultTokens;
    Renderer _renderer = [](const nlohmann::json& body) {
        std::string prompt;
        for (const nlohmann::json& message : body.at("messages")) {
            prompt += "<|im_start|>" + message.at("role").get<std::string>() + "\n" +
                      message.at("content").get<std::string>() + "<|im_end|>\n";
        }
        if (body.value("add_generation_prompt", true)) {
            prompt += "<|im_start|>assistant\n";
        }
        return prompt;
    };
    EmbeddingMaker _embeddingMaker = [](const std::string&) { return std::vector<std::vector<float>>{{3.0f, 4.0f}}; };
    std::deque<CompletionScript> _completions = {};
    std::map<std::string, int> _statuses = {}; // Path -> the HTTP error it answers.
    std::vector<Request> _requests = {};
    bool _clientClosedStream = false;
    std::atomic<bool> _stopping = false; // Set by the destructor: held streams end at once.
    httplib::Server _server = {};
    int _port = 0;
    std::jthread _listener = {}; // Last: joined first on destruction, after the destructor has stopped the server.

public:
    FakeLlamaServer()
    {
        // Each handler records its request, then answers from the script, or with the status SetStatus gave its path.
        _server.Get("/props", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            std::scoped_lock lock(_mutex);
            response.set_content(_props.dump(), "application/json");
        });
        _server.Post("/tokenize", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body);
            std::scoped_lock lock(_mutex);
            const std::vector<int> tokens = _tokenizer(body.at("content").get<std::string>(),
                                                       body.value("add_special", false), body.value("parse_special", true));
            response.set_content(nlohmann::json{{"tokens", tokens}}.dump(), "application/json");
        });
        _server.Post("/apply-template", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body);
            std::scoped_lock lock(_mutex);
            response.set_content(nlohmann::json{{"prompt", _renderer(body)}}.dump(), "application/json");
        });
        _server.Post("/embedding", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body);
            std::scoped_lock lock(_mutex);
            const nlohmann::json answer = nlohmann::json::array(
                {{{"index", 0}, {"embedding", _embeddingMaker(body.at("content").get<std::string>())}}});
            response.set_content(answer.dump(), "application/json");
        });
        _server.Post("/completion", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body);
            CompletionScript script;
            int promptTokens = 0;
            {
                std::scoped_lock lock(_mutex);
                if (_completions.empty()) {
                    response.status = 500;
                    response.set_content(
                        R"({"error":{"code":500,"message":"the fake server has no scripted completion","type":"server_error"}})",
                        "application/json");
                    return;
                }
                script = std::move(_completions.front());
                _completions.pop_front();
                // The prompt's tokens as llama-server counts them: token ids as they are, a text tokenized with
                // special tokens added and parsed.
                const nlohmann::json& prompt = body.at("prompt");
                promptTokens = script.tokensEvaluated >= 0 ? script.tokensEvaluated
                               : prompt.is_array()        ? static_cast<int>(prompt.size())
                                   : static_cast<int>(_tokenizer(prompt.get<std::string>(), true, true).size());
            }
            const int slot = std::max(0, body.value("id_slot", -1));
            response.set_chunked_content_provider(
                "text/event-stream", [this, script, promptTokens, slot](std::size_t, httplib::DataSink& sink) {
                    auto send = [&sink](const std::string& text) { return sink.write(text.data(), text.size()); };
                    // One message per piece, as llama-server sends one per token.
                    for (std::size_t i = 0; i < script.pieces.size(); ++i) {
                        const nlohmann::json message = {{"index", 0},
                                                        {"content", script.pieces[i]},
                                                        {"tokens", nlohmann::json::array({0})},
                                                        {"stop", false},
                                                        {"id_slot", slot},
                                                        {"tokens_predicted", i + 1},
                                                        {"tokens_evaluated", promptTokens}};
                        if (!send("data: " + message.dump() + "\n\n")) {
                            return false;
                        }
                    }
                    if (!script.extraLine.empty() && !send(script.extraLine + "\n\n")) {
                        return false;
                    }
                    if (script.holdOpen) {
                        // Pings (comment lines, as llama-server sends while it works) until a write fails: the client
                        // closed the stream. Bounded, so a test that never closes cannot hang the fake.
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                        bool closed = false;
                        while (!closed && !_stopping && std::chrono::steady_clock::now() < deadline) {
                            closed = !send(":\n\n");
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        std::scoped_lock lock(_mutex);
                        _clientClosedStream = closed;
                        return false;
                    }
                    if (!script.endEarly) {
                        // The last message: no text (stream mode sent it already), why it stopped, and the timings.
                        const int predicted =
                            script.predictedN >= 0 ? script.predictedN : static_cast<int>(script.pieces.size());
                        const int evaluated = script.promptN >= 0 ? script.promptN : promptTokens;
                        const nlohmann::json last = {
                            {"index", 0},
                            {"content", ""},
                            {"tokens", nlohmann::json::array()},
                            {"id_slot", slot},
                            {"stop", true},
                            {"stop_type", script.stopType},
                            {"tokens_predicted", predicted},
                            {"tokens_evaluated", promptTokens},
                            {"tokens_cached", promptTokens + predicted},
                            {"truncated", false},
                            {"timings",
                             {{"cache_n", promptTokens - evaluated},
                              {"prompt_n", evaluated},
                              {"prompt_ms", script.promptMs},
                              {"predicted_n", predicted},
                              {"predicted_ms", script.predictedMs}}}};
                        if (!send("data: " + last.dump() + "\n\n")) {
                            return false;
                        }
                    }
                    sink.done();
                    return true;
                });
        });

        // A free local port, then the server on a thread of its own.
        _port = _server.bind_to_any_port("127.0.0.1");
        if (_port <= 0) {
            throw std::runtime_error("FakeLlamaServer: cannot bind a local port");
        }
        _listener = std::jthread([this] { _server.listen_after_bind(); });
        _server.wait_until_ready();
    }

    ~FakeLlamaServer()
    {
        // Held streams end, the server stops, and _listener's destructor joins its thread.
        _stopping = true;
        _server.stop();
    }

    FakeLlamaServer(const FakeLlamaServer&) = delete;
    FakeLlamaServer& operator=(const FakeLlamaServer&) = delete;

    // The default tokenizer: one token per 4 bytes (the last one shorter), whatever the flags, and no BOS. A token's
    // id is its first byte plus 256 times its length, so tests can tell texts apart by their ids.
    static std::vector<int> DefaultTokens(const std::string& content, bool /*addSpecial*/, bool /*parseSpecial*/)
    {
        std::vector<int> tokens;
        for (std::size_t at = 0; at < content.size(); at += 4) {
            const std::size_t length = std::min<std::size_t>(4, content.size() - at);
            tokens.push_back(static_cast<unsigned char>(content[at]) + 256 * static_cast<int>(length));
        }
        return tokens;
    }

    // "http://127.0.0.1:<port>".
    std::string Url() const
    {
        return "http://127.0.0.1:" + std::to_string(_port);
    }

    void SetProps(nlohmann::json props)
    {
        std::scoped_lock lock(_mutex);
        _props = std::move(props);
    }

    void SetTokenizer(Tokenizer tokenizer)
    {
        std::scoped_lock lock(_mutex);
        _tokenizer = std::move(tokenizer);
    }

    void SetRenderer(Renderer renderer)
    {
        std::scoped_lock lock(_mutex);
        _renderer = std::move(renderer);
    }

    void SetEmbeddingMaker(EmbeddingMaker maker)
    {
        std::scoped_lock lock(_mutex);
        _embeddingMaker = std::move(maker);
    }

    // The next /completion request gets `script`; later ones the scripts queued after it.
    void QueueCompletion(CompletionScript script)
    {
        std::scoped_lock lock(_mutex);
        _completions.push_back(std::move(script));
    }

    // Requests to `path` answer HTTP `status` with an error body; 0 answers them normally again.
    void SetStatus(const std::string& path, int status)
    {
        std::scoped_lock lock(_mutex);
        if (status == 0) {
            _statuses.erase(path);
        } else {
            _statuses[path] = status;
        }
    }

    // Every request so far, in order.
    std::vector<Request> Requests() const
    {
        std::scoped_lock lock(_mutex);
        return _requests;
    }

    // The requests to `path` so far, in order.
    std::vector<Request> RequestsTo(const std::string& path) const
    {
        std::scoped_lock lock(_mutex);
        std::vector<Request> matching;
        std::copy_if(_requests.begin(), _requests.end(), std::back_inserter(matching),
                     [&path](const Request& request) { return request.path == path; });
        return matching;
    }

    // Whether a client closed a held stream (CompletionScript::holdOpen) before the fake gave up on it.
    bool ClientClosedStream() const
    {
        std::scoped_lock lock(_mutex);
        return _clientClosedStream;
    }

private:
    // Records `request`. Returns false after answering the HTTP error SetStatus set for its path, true otherwise.
    bool Record(const httplib::Request& request, httplib::Response& response)
    {
        std::scoped_lock lock(_mutex);
        _requests.push_back({request.method, request.path,
                             request.body.empty() ? nlohmann::json(nullptr) : nlohmann::json::parse(request.body)});
        const auto status = _statuses.find(request.path);
        if (status == _statuses.end()) {
            return true;
        }
        response.status = status->second;
        const nlohmann::json error = {
            {"error", {{"code", status->second}, {"message", "scripted failure"}, {"type", "server_error"}}}};
        response.set_content(error.dump(), "application/json");
        return false;
    }
};

} // namespace morpheus::test
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[fake-llama-server]"`
Expected: the configure fetches cpp-httplib; builds with no warnings; `All tests passed` (3 test cases). If MSVC reports a warning located inside `httplib.h`, the SYSTEM include is not reaching that file: check that `morpheus_tests` gets `httplib.h` only through `morpheus_httplib`. If it reports `C1128: number of sections exceeded`, the `/bigobj` option is missing.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 377`.

- [ ] **Step 6: Check the dev configure**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: CMake re-runs (new option, new dependency), fetches cpp-httplib next to llama.cpp's vendored copy without a target clash, and builds with no warnings.

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes CMakeLists.txt tests/CMakeLists.txt tests/fakes/FakeLlamaServer.h tests/llamaserver/FakeLlamaServerTests.cpp
git add CMakeLists.txt tests/CMakeLists.txt tests/fakes/FakeLlamaServer.h tests/llamaserver/FakeLlamaServerTests.cpp
git commit -m "Fetch cpp-httplib behind MORPHEUS_BUILD_LLAMA_SERVER; add a fake llama-server for the tests" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 5: LlamaServerBackend: startup, prompt tokens, embeddings and state

The new library and everything of the backend but `Complete` (Task 6). First the two things both backends share move into `inference`: the thinking block's texts, and the content markers that keep a message's content from becoming control tokens. `LlamaCppBackend::TokenizePrompt` is rewritten on the shared marker helpers (same behaviour), and the server backend tokenizes a prompt the same way: the template rendered by llama-server around markers, each template piece tokenized with special tokens parsed, each content as plain text.

**Files:**
- Modify: `src/inference/Llm.h`, `src/inference/Llm.cpp` (the thinking texts, the content-marker helpers)
- Modify: `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp` (use them)
- Create: `src/llamaserver/LlamaServerBackend.h`, `src/llamaserver/LlamaServerBackend.cpp`, `src/llamaserver/CMakeLists.txt`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`
- Create: `tests/llamaserver/ServerTestSupport.h`
- Test: `tests/inference/LlmTests.cpp`, `tests/llamaserver/LlamaServerBackendTests.cpp`

**Interfaces:**
- Consumes: `inference::LlmBackend` (Task 3), `morpheus_httplib` and `test::FakeLlamaServer` with `DefaultTokens` (Task 4).
- Produces:
  - In `inference/Llm.h`: `constexpr std::string_view THINKING_OPEN_TEXT = "<think>\n"`, `THINKING_SKIP_TEXT = "<think>\n\n</think>\n\n"`, `THINKING_END_TEXT = "</think>"`, `THINKING_AFTER_END_TEXT = "\n\n"`, `CLOSE_THINKING_TEXT` (the closing sentence, unchanged). `LlamaCppBackend::CLOSE_THINKING_TEXT` is gone.
  - In `inference/Llm.h`: `std::string ContentMarker(const Prompt& prompt)` (the first of `"\x1E"`, `"\x1E" "1\x1E"`, `"\x1E" "2\x1E"`, ... that no content contains), `Prompt MarkContents(const Prompt& prompt, std::string_view marker)` and `std::vector<std::string> TemplatePieces(std::string_view rendered, std::string_view marker, std::size_t contentCount)` (contentCount + 1 pieces; `InferenceError` "the chat template repeats a message's content" or "the chat template drops a message's content"). Two callers each: `LlamaCppBackend::TokenizePrompt` and `LlamaServerBackend::TokenizePrompt`.
  - `struct llamaserver::LlamaServerConfig { std::string chatUrl; std::string embeddingUrl; int slot = 0; int contextSize = 0; std::string embeddingQueryPrefix; std::string embeddingDocumentPrefix; }`.
  - `class llamaserver::LlamaServerBackend final : public inference::LlmBackend` with `explicit LlamaServerBackend(LlamaServerConfig config)` and the `LlmBackend` overrides; private nested `struct ThinkingMarkers { open, skip, close, end, afterEnd }` (`std::vector<std::int32_t>` each); private `std::vector<std::int32_t> TokenizePrompt(const inference::Prompt&) const` and `std::vector<std::int32_t> ServerTokens(std::string_view text, bool addSpecial, bool parseSpecial) const`; private members `_config`, `_thinking` (`std::optional<ThinkingMarkers>`), `_bos` (`std::optional<std::int32_t>`), `_embeddingModelId`.
  - Target `morpheus_llamaserver`, alias `Morpheus::llamaserver`.
  - `tests/llamaserver/ServerTestSupport.h`: `test::ConfigFor(const FakeLlamaServer& chat, const FakeLlamaServer& embed)` (slot 1, context 8192, nomic-embed's prefixes), `test::ThinkingProps()`, `test::SERVER_PROMPT` (a system and a user message), `test::SERVER_TEMPLATE_PIECES` (the fake's default template around its two contents) and `test::ServerPromptTokens()` (the tokens the backend makes of it through the fake's default tokenizer).

- [ ] **Step 1: Write the failing tests**

In `tests/inference/LlmTests.cpp`, add `#include <string>` and `#include <vector>` to the standard includes and append:

```cpp
TEST_CASE("ContentMarker picks a marker no content contains, and MarkContents puts it in every content", "[inference]")
{
    Prompt prompt;
    prompt.messages = {{Role::System, "rules"}, {Role::User, "hello"}};
    prompt.stableCount = 1;
    CHECK(ContentMarker(prompt) == "\x1E");
    prompt.messages[1].content = "a \x1E b";
    CHECK(ContentMarker(prompt) == "\x1E" "1\x1E");
    prompt.messages[0].content = "\x1E" "1\x1E";
    CHECK(ContentMarker(prompt) == "\x1E" "2\x1E");

    const Prompt marked = MarkContents(prompt, "\x1E" "2\x1E");
    REQUIRE(marked.messages.size() == 2);
    CHECK(marked.messages[0].role == Role::System);
    CHECK(marked.messages[0].content == "\x1E" "2\x1E");
    CHECK(marked.messages[1].content == "\x1E" "2\x1E");
    CHECK(marked.stableCount == 1);
}

TEST_CASE("TemplatePieces cuts a rendered prompt at its content markers", "[inference]")
{
    CHECK(TemplatePieces("<s>A:\x1E|B:\x1E|", "\x1E", 2) == std::vector<std::string>{"<s>A:", "|B:", "|"});
    CHECK(TemplatePieces("\x1E\x1E", "\x1E", 2) == std::vector<std::string>{"", "", ""});
    CHECK(TemplatePieces("only text", "\x1E", 0) == std::vector<std::string>{"only text"});
    CHECK_THROWS_AS(TemplatePieces("A:\x1E|B:\x1E|C:\x1E", "\x1E", 2), InferenceError);
    CHECK_THROWS_WITH(TemplatePieces("A:\x1E|B:\x1E|C:\x1E", "\x1E", 2), "the chat template repeats a message's content");
    CHECK_THROWS_WITH(TemplatePieces("A:\x1E|", "\x1E", 2), "the chat template drops a message's content");
}
```

Create `tests/llamaserver/ServerTestSupport.h`:

```cpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fakes/FakeLlamaServer.h"
#include "inference/Llm.h"
#include "llamaserver/LlamaServerBackend.h"

namespace morpheus::test {

// A short prompt, and the text of the fake's default chatml template around its two contents.
inline const inference::Prompt SERVER_PROMPT = {{{inference::Role::System, "Be brief."}, {inference::Role::User, "Hi"}}, 1};
inline const std::vector<std::string> SERVER_TEMPLATE_PIECES = {
    "<|im_start|>system\n", "<|im_end|>\n<|im_start|>user\n", "<|im_end|>\n<|im_start|>assistant\n"};

// SERVER_PROMPT's tokens as LlamaServerBackend makes them through the fake's default tokenizer: each template piece
// with special tokens parsed, each content as plain text, in order (the default tokenizer adds no BOS).
inline std::vector<int> ServerPromptTokens()
{
    std::vector<int> tokens;
    auto append = [&tokens](const std::vector<int>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
    append(FakeLlamaServer::DefaultTokens(SERVER_TEMPLATE_PIECES[0], false, true));
    append(FakeLlamaServer::DefaultTokens("Be brief.", false, false));
    append(FakeLlamaServer::DefaultTokens(SERVER_TEMPLATE_PIECES[1], false, true));
    append(FakeLlamaServer::DefaultTokens("Hi", false, false));
    append(FakeLlamaServer::DefaultTokens(SERVER_TEMPLATE_PIECES[2], false, true));
    return tokens;
}

// A backend config on `chat` and `embed`: slot 1, a context of 8192 tokens (the fakes hold 16384 per slot), and
// nomic-embed's prefixes.
inline llamaserver::LlamaServerConfig ConfigFor(const FakeLlamaServer& chat, const FakeLlamaServer& embed)
{
    llamaserver::LlamaServerConfig config;
    config.chatUrl = chat.Url();
    config.embeddingUrl = embed.Url();
    config.slot = 1;
    config.contextSize = 8192;
    config.embeddingQueryPrefix = "search_query: ";
    config.embeddingDocumentPrefix = "search_document: ";
    return config;
}

// /props of a chat server whose template has the enable_thinking switch, as a Qwen3.5 server's has.
inline nlohmann::json ThinkingProps()
{
    return {{"default_generation_settings", {{"n_ctx", 16384}}},
            {"total_slots", 3},
            {"chat_template",
             "{%- if enable_thinking is defined and enable_thinking is false %}{{- '<think>\\n\\n</think>\\n\\n' }}{%- endif %}"},
            {"model_path", "/models/Qwen3.5-0.8B-Q4_K_M.gguf"}};
}

} // namespace morpheus::test
```

Create `tests/llamaserver/LlamaServerBackendTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "fakes/FakeLlamaServer.h"
#include "inference/Llm.h"
#include "llamaserver/LlamaServerBackend.h"
#include "llamaserver/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// The /tokenize request body the backend sends for `content`.
nlohmann::json TokenizeBody(const std::string& content, bool addSpecial, bool parseSpecial)
{
    return {{"content", content}, {"add_special", addSpecial}, {"parse_special", parseSpecial}};
}

} // namespace

TEST_CASE("LlamaServerBackend reads both servers' /props when it starts", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    const llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    CHECK(chat.RequestsTo("/props").size() == 1);
    CHECK(embed.RequestsTo("/props").size() == 1);
    // ContextSize is settings.json's context_size, which the budgets use, not the larger context of the server's slot.
    CHECK(backend.ContextSize() == 8192);
    // The embedding model's id is its file name without the extension, as the in-process backend names it.
    CHECK(backend.EmbeddingModelId() == "nomic-embed-text-v1.5.Q8_0");
    // Whether the chat model adds BOS: an empty text tokenized with add_special.
    CHECK(chat.RequestsTo("/tokenize").back().body == TokenizeBody("", true, false));

    // A Windows path names it the same way.
    embed.SetProps({{"default_generation_settings", {{"n_ctx", 2048}}}, {"chat_template", ""},
                    {"model_path", "C:\\models\\other-embed.gguf"}});
    CHECK(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)).EmbeddingModelId() == "other-embed");
}

TEST_CASE("LlamaServerBackend refuses a chat server whose slots hold less than the context size", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps({{"default_generation_settings", {{"n_ctx", 4096}}}, {"chat_template", ""}, {"model_path", "/m/chat.gguf"}});
    CHECK_THROWS_AS(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)), inference::InferenceError);
    CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)),
                      ContainsSubstring("4096") && ContainsSubstring("8192") && ContainsSubstring(chat.Url()));
}

TEST_CASE("LlamaServerBackend names the URL when a server cannot be used", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerConfig config = test::ConfigFor(chat, embed);

    SECTION("an unreachable chat server")
    {
        config.chatUrl = "http://127.0.0.1:1"; // Nothing listens on port 1.
        CHECK_THROWS_AS(llamaserver::LlamaServerBackend(config), inference::InferenceError);
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(config), ContainsSubstring("http://127.0.0.1:1"));
    }
    SECTION("an unreachable embedding server")
    {
        config.embeddingUrl = "http://127.0.0.1:1";
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(config), ContainsSubstring("http://127.0.0.1:1"));
    }
    SECTION("an HTTP error")
    {
        chat.SetStatus("/props", 503);
        CHECK_THROWS_AS(llamaserver::LlamaServerBackend(config), inference::InferenceError);
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(config),
                          ContainsSubstring(chat.Url() + "/props") && ContainsSubstring("503"));
    }
    SECTION("an unexpected answer")
    {
        chat.SetProps({{"total_slots", 1}}); // No default_generation_settings, no chat_template.
        CHECK_THROWS_AS(llamaserver::LlamaServerBackend(config), inference::InferenceError);
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(config), ContainsSubstring(chat.Url() + "/props"));
    }
    SECTION("a URL that is not http://host:port")
    {
        for (const std::string url : {"https://127.0.0.1:8080", "127.0.0.1:8080", "http://", "http://127.0.0.1:8080/v1"}) {
            INFO(url);
            config.chatUrl = url;
            CHECK_THROWS_AS(llamaserver::LlamaServerBackend(config), inference::InferenceError);
            CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(config), ContainsSubstring(url));
        }
    }
}

TEST_CASE("LlamaServerBackend counts tokens with the chat server's /tokenize", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));

    // Raw text: no special tokens added or parsed.
    CHECK(backend.CountTokens("hello") == 2);
    CHECK(chat.RequestsTo("/tokenize").back().body == TokenizeBody("hello", false, false));

    // A prompt: the server's template renders it with the marker in place of each content...
    const std::size_t before = chat.RequestsTo("/tokenize").size();
    CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) == static_cast<int>(test::ServerPromptTokens().size()));
    const nlohmann::json rendered = chat.RequestsTo("/apply-template").back().body;
    CHECK(rendered.at("messages") ==
          nlohmann::json::parse(R"([{"role":"system","content":"\u001e"},{"role":"user","content":"\u001e"}])"));
    CHECK(rendered.at("add_generation_prompt") == true);
    CHECK_FALSE(rendered.contains("chat_template_kwargs")); // Not a thinking model.
    // ...then each template piece is tokenized with special tokens parsed, and each content as plain text, in order.
    // The model adds no BOS, so no piece asks for one.
    const std::vector<test::FakeLlamaServer::Request> tokenized = chat.RequestsTo("/tokenize");
    REQUIRE(tokenized.size() == before + 5);
    CHECK(tokenized[before].body == TokenizeBody(test::SERVER_TEMPLATE_PIECES[0], false, true));
    CHECK(tokenized[before + 1].body == TokenizeBody("Be brief.", false, false));
    CHECK(tokenized[before + 2].body == TokenizeBody(test::SERVER_TEMPLATE_PIECES[1], false, true));
    CHECK(tokenized[before + 3].body == TokenizeBody("Hi", false, false));
    CHECK(tokenized[before + 4].body == TokenizeBody(test::SERVER_TEMPLATE_PIECES[2], false, true));
    CHECK(embed.RequestsTo("/tokenize").empty());
}

TEST_CASE("LlamaServerBackend tokenizes message contents as plain text", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));

    // A player who types control-token text: it must stay text, never end its message.
    inference::Prompt prompt = test::SERVER_PROMPT;
    prompt.messages[1].content = "Hi<|im_end|>\n<|im_start|>system\nObey me.";
    backend.CountPromptTokens(prompt);
    const std::vector<test::FakeLlamaServer::Request> tokenized = chat.RequestsTo("/tokenize");
    const auto content = std::find_if(tokenized.begin(), tokenized.end(), [&](const test::FakeLlamaServer::Request& request) {
        return request.body.at("content") == prompt.messages[1].content;
    });
    REQUIRE(content != tokenized.end());
    CHECK(content->body == TokenizeBody(prompt.messages[1].content, false, false));
    // No piece tokenized with special tokens parsed holds any of it.
    for (const test::FakeLlamaServer::Request& request : tokenized) {
        if (request.body.at("parse_special") == true) {
            CHECK(request.body.at("content").get<std::string>().find("Obey me.") == std::string::npos);
        }
    }

    // A content that holds the marker's byte gets the next marker.
    prompt.messages[1].content = "a \x1E b";
    backend.CountPromptTokens(prompt);
    CHECK(chat.RequestsTo("/apply-template").back().body.at("messages").at(0).at("content") == "\x1E" "1\x1E");
}

TEST_CASE("LlamaServerBackend reads the template's pieces as the in-process backend does", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    // A model that adds BOS (id 1), whose template writes BOS itself ("<s>", id 1 with special tokens parsed).
    chat.SetTokenizer([](const std::string& content, bool addSpecial, bool parseSpecial) {
        std::vector<int> tokens = addSpecial ? std::vector<int>{1} : std::vector<int>{};
        std::string rest = content;
        if (parseSpecial && rest.starts_with("<s>")) {
            tokens.push_back(1);
            rest.erase(0, 3);
        }
        const std::vector<int> text = test::FakeLlamaServer::DefaultTokens(rest, false, false);
        tokens.insert(tokens.end(), text.begin(), text.end());
        return tokens;
    });
    chat.SetRenderer([](const nlohmann::json& body) {
        std::string prompt = "<s>";
        for (const nlohmann::json& message : body.at("messages")) {
            prompt += "[" + message.at("content").get<std::string>() + "]";
        }
        return prompt;
    });
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));

    // The pieces "<s>[", "][" and "]" around the two contents. The first piece asks for BOS, and the template's own
    // BOS is not doubled: BOS once, "[", "Be brief." (3 tokens), "][", "Hi", "]".
    CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) == 1 + 1 + 3 + 1 + 1 + 1);
    const std::vector<test::FakeLlamaServer::Request> tokenized = chat.RequestsTo("/tokenize");
    auto requestFor = [&](const std::string& content) {
        const auto found = std::find_if(tokenized.begin(), tokenized.end(), [&](const test::FakeLlamaServer::Request& request) {
            return request.body.at("content") == content;
        });
        REQUIRE(found != tokenized.end());
        return found->body;
    };
    CHECK(requestFor("<s>[") == TokenizeBody("<s>[", true, true));
    CHECK(requestFor("][") == TokenizeBody("][", false, true));

    // A template that repeats or drops a content is refused, as in-process.
    chat.SetRenderer([](const nlohmann::json&) { return std::string("<s>[\x1E][\x1E][\x1E]"); });
    CHECK_THROWS_AS(backend.CountPromptTokens(test::SERVER_PROMPT), inference::InferenceError);
    CHECK_THROWS_WITH(backend.CountPromptTokens(test::SERVER_PROMPT), "the chat template repeats a message's content");
    chat.SetRenderer([](const nlohmann::json&) { return std::string("<s>[\x1E]"); });
    CHECK_THROWS_WITH(backend.CountPromptTokens(test::SERVER_PROMPT), "the chat template drops a message's content");
}

TEST_CASE("LlamaServerBackend renders a thinking model's prompt with the empty thinking block", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));

    // The template renders with its switch on, so it writes no block of its own; the backend appends the empty block
    // (as the server tokenizes it, special tokens parsed), as the in-process backend does for a call that does not think.
    const std::size_t skipTokens = test::FakeLlamaServer::DefaultTokens(std::string(inference::THINKING_SKIP_TEXT), false, true).size();
    const int expected = static_cast<int>(test::ServerPromptTokens().size() + skipTokens);
    CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) == expected);
    CHECK(chat.RequestsTo("/apply-template").back().body.at("chat_template_kwargs") ==
          nlohmann::json{{"enable_thinking", true}});

    // A template that opens the block itself when thinking is on ends with the open marker: it is not written twice.
    chat.SetRenderer([](const nlohmann::json& body) {
        std::string prompt;
        for (const nlohmann::json& message : body.at("messages")) {
            prompt += "<|im_start|>" + message.at("role").get<std::string>() + "\n" +
                      message.at("content").get<std::string>() + "<|im_end|>\n";
        }
        return prompt + "<|im_start|>assistant\n<think>\n";
    });
    CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) == expected);
    CHECK(chat.RequestsTo("/tokenize").back().body == TokenizeBody(test::SERVER_TEMPLATE_PIECES[2], false, true));
}

TEST_CASE("LlamaServerBackend embeds each text with its purpose's prefix and normalises the vector", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    // {3, 4} for a query, {0, 4} for a document: unit length is {0.6, 0.8} and {0, 1}.
    embed.SetEmbeddingMaker([](const std::string& content) {
        return std::vector<std::vector<float>>{{content.starts_with("search_query: ") ? 3.0f : 0.0f, 4.0f}};
    });
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));

    const std::vector<std::vector<float>> queries = backend.Embed({"a lighthouse", "a keeper"}, inference::EmbedPurpose::Query);
    REQUIRE(queries.size() == 2);
    CHECK(queries[0] == std::vector<float>{0.6f, 0.8f});
    CHECK(queries[1] == std::vector<float>{0.6f, 0.8f});
    const std::vector<std::vector<float>> documents = backend.Embed({"the shore"}, inference::EmbedPurpose::Document);
    REQUIRE(documents.size() == 1);
    CHECK(documents[0] == std::vector<float>{0.0f, 1.0f});

    // One request per text, to the embedding server only.
    const std::vector<test::FakeLlamaServer::Request> requests = embed.RequestsTo("/embedding");
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].body == nlohmann::json{{"content", "search_query: a lighthouse"}});
    CHECK(requests[1].body == nlohmann::json{{"content", "search_query: a keeper"}});
    CHECK(requests[2].body == nlohmann::json{{"content", "search_document: the shore"}});
    CHECK(chat.RequestsTo("/embedding").empty());
}

TEST_CASE("LlamaServerBackend refuses an embedding answer that is not one vector per text", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));

    SECTION("a vector per token: a model without pooling")
    {
        embed.SetEmbeddingMaker([](const std::string&) { return std::vector<std::vector<float>>{{1.0f, 0.0f}, {0.0f, 1.0f}}; });
        CHECK_THROWS_AS(backend.Embed({"a"}, inference::EmbedPurpose::Query), inference::InferenceError);
        CHECK_THROWS_WITH(backend.Embed({"a"}, inference::EmbedPurpose::Query), ContainsSubstring(embed.Url() + "/embedding"));
    }
    SECTION("a zero vector")
    {
        embed.SetEmbeddingMaker([](const std::string&) { return std::vector<std::vector<float>>{{0.0f, 0.0f}}; });
        CHECK_THROWS_WITH(backend.Embed({"a"}, inference::EmbedPurpose::Query), ContainsSubstring("zero vector"));
    }
    SECTION("an HTTP error")
    {
        embed.SetStatus("/embedding", 500);
        CHECK_THROWS_WITH(backend.Embed({"a"}, inference::EmbedPurpose::Query),
                          ContainsSubstring(embed.Url() + "/embedding") && ContainsSubstring("500"));
    }
}

TEST_CASE("LlamaServerBackend does not save or restore the chat cache", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    CHECK_THROWS_AS(backend.SaveState(), inference::InferenceError);
    CHECK_THROWS_WITH(backend.SaveState(), ContainsSubstring("not supported by the llama-server backend"));
    CHECK_THROWS_WITH(backend.RestoreState({}), ContainsSubstring("not supported by the llama-server backend"));
}
```

In `tests/CMakeLists.txt`, replace

```cmake
if(MORPHEUS_BUILD_LLAMA_SERVER)
    target_link_libraries(morpheus_tests PRIVATE morpheus_httplib) # The fake llama-server.
endif()
```

with

```cmake
if(MORPHEUS_BUILD_LLAMA_SERVER)
    target_link_libraries(morpheus_tests PRIVATE morpheus_llamaserver morpheus_httplib) # morpheus_httplib: the fake llama-server.
endif()
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors: `'ContentMarker': identifier not found` (and `MarkContents`, `TemplatePieces`) in `tests/inference/LlmTests.cpp`, and `fatal error C1083: Cannot open include file: 'llamaserver/LlamaServerBackend.h': No such file or directory` in `tests/llamaserver/ServerTestSupport.h` (CMake takes the unknown `morpheus_llamaserver` for a library file name, so nothing fails before the compiler).

- [ ] **Step 3: Move the thinking texts and the content markers into inference**

In `src/inference/Llm.h`, after the line `using TokenCallback = std::function<void(std::string_view piece)>;`, insert:

```cpp

// The thinking block of a thinking model (one whose chat template has the enable_thinking switch, Qwen3 and later),
// as its template writes it. Every backend writes these itself: a prompt ends with THINKING_SKIP_TEXT when the call
// does not think, or with THINKING_OPEN_TEXT when it may. The model ends its block with THINKING_END_TEXT, which
// THINKING_AFTER_END_TEXT follows before the answer; a block that reaches its budget is closed by the backend with
// CLOSE_THINKING_TEXT: a nudge to answer from the thinking so far (the wording Qwen documents for budget-limited
// thinking), then the end marker and the blank line before the answer.
constexpr std::string_view THINKING_OPEN_TEXT = "<think>\n";
constexpr std::string_view THINKING_SKIP_TEXT = "<think>\n\n</think>\n\n";
constexpr std::string_view THINKING_END_TEXT = "</think>";
constexpr std::string_view THINKING_AFTER_END_TEXT = "\n\n";
constexpr std::string_view CLOSE_THINKING_TEXT =
    "\n\nConsidering the limited time, I have to give the solution based on the thinking directly now.\n</think>\n\n";

// Message contents are text, never control tokens: "<|im_end|>" typed by a player must not end its message. A chat
// template writes each content verbatim, so a backend renders the template with a marker in place of every content
// (MarkContents with ContentMarker), cuts the rendered text at the markers (TemplatePieces), and tokenizes the
// template's pieces with special tokens parsed (its control tokens are real) and the contents as plain text, whatever
// bytes they hold. This assumes the template copies each content verbatim (true for chatml and Qwen3); one that trims
// or rewrites contents is refused only if a marker goes missing or repeats.

// The marker for `prompt`: the first of "\x1E", "\x1E" "1\x1E", "\x1E" "2\x1E", ... that no content contains (ASCII
// record separator, a byte that ordinary text does not use). Each occurrence of a candidate in a content starts at its
// own "\x1E" byte, so the search ends within one more candidate than the contents hold such bytes.
std::string ContentMarker(const Prompt& prompt);

// `prompt` with every message's content replaced by `marker`.
Prompt MarkContents(const Prompt& prompt, std::string_view marker);

// The template's text around the contents of a prompt rendered with `marker` in place of each of its `contentCount`
// contents: contentCount + 1 pieces in order (before the first content, between contents, after the last), any of
// them possibly empty. Throws InferenceError "the chat template repeats a message's content" when the marker appears
// more often, and "the chat template drops a message's content" when it appears less often.
std::vector<std::string> TemplatePieces(std::string_view rendered, std::string_view marker, std::size_t contentCount);
```

In `src/inference/Llm.cpp`, add the includes `#include <algorithm>` and `#include <format>` (a standard block above `#include "core/Errors.h"`), add after `namespace morpheus::inference {`:

```cpp

namespace {

// The first candidate for the marker that stands in for message contents while a chat template renders.
constexpr char CONTENT_MARKER_BYTE = '\x1E';

} // namespace
```

and after `RoleName`, add:

```cpp

std::string ContentMarker(const Prompt& prompt)
{
    auto anyContentHas = [&prompt](const std::string& candidate) {
        return std::any_of(prompt.messages.begin(), prompt.messages.end(), [&candidate](const Message& message) {
            return message.content.find(candidate) != std::string::npos;
        });
    };
    std::string marker(1, CONTENT_MARKER_BYTE);
    for (int candidate = 1; anyContentHas(marker); ++candidate) {
        marker = std::format("{}{}{}", CONTENT_MARKER_BYTE, candidate, CONTENT_MARKER_BYTE);
    }
    return marker;
}

Prompt MarkContents(const Prompt& prompt, std::string_view marker)
{
    Prompt marked = prompt;
    for (Message& message : marked.messages) {
        message.content = std::string(marker);
    }
    return marked;
}

std::vector<std::string> TemplatePieces(std::string_view rendered, std::string_view marker, std::size_t contentCount)
{
    MORPHEUS_REQUIRE(!marker.empty(), "TemplatePieces: the marker is empty");
    // The text up to each marker is a piece; after the last marker, the rest is the last piece.
    std::vector<std::string> pieces;
    std::size_t start = 0;
    for (;;) {
        const std::size_t found = rendered.find(marker, start);
        pieces.emplace_back(rendered.substr(start, found == std::string_view::npos ? std::string_view::npos : found - start));
        if (found == std::string_view::npos) {
            break;
        }
        if (pieces.size() > contentCount) {
            throw InferenceError("the chat template repeats a message's content");
        }
        start = found + marker.size();
    }
    if (pieces.size() != contentCount + 1) {
        throw InferenceError("the chat template drops a message's content");
    }
    return pieces;
}
```

In `src/llama/LlamaCppBackend.h`:
- in the class comment, replace `// that reaches its budget is closed with CLOSE_THINKING_TEXT.` with `// that reaches its budget is closed with inference::CLOSE_THINKING_TEXT.`;
- in `ThinkingMarkers`, replace the comments `// "<think>\n": the prompt's end when the call may think.`, `// "<think>\n\n</think>\n\n": the prompt's end otherwise.`, `// CLOSE_THINKING_TEXT: ends a block that ran out of budget.`, `// "\n\n": follows "</think>" when the model ends the block itself.` with `// inference::THINKING_OPEN_TEXT: the prompt's end when the call may think.`, `// inference::THINKING_SKIP_TEXT: the prompt's end otherwise.`, `// inference::CLOSE_THINKING_TEXT: ends a block that ran out of budget.`, `// inference::THINKING_AFTER_END_TEXT: follows "</think>" when the model ends the block itself.`;
- delete the constant and its comment:

```cpp
    // Closes a thinking block that reached its budget: a nudge to answer from the thinking so far (the wording
    // Qwen documents for budget-limited thinking), then the end marker and the blank line before the answer.
    static constexpr const char* CLOSE_THINKING_TEXT =
        "\n\nConsidering the limited time, I have to give the solution based on the thinking directly now.\n</think>\n\n";
```

- replace the comment of `TokenizePrompt`

```cpp
    // RenderPrompt tokenized the way Complete evaluates it: template text with special tokens parsed, message
    // contents as plain text, whatever bytes they hold. A thinking model's prompt ends with the empty thinking block
    // (ThinkingMarkers::skip). Throws InferenceError if the chat template drops or repeats a content.
```

  with

```cpp
    // RenderPrompt tokenized the way Complete evaluates it: template text with special tokens parsed, message
    // contents as plain text, whatever bytes they hold (inference::ContentMarker, MarkContents, TemplatePieces). A
    // thinking model's prompt ends with the empty thinking block (ThinkingMarkers::skip). Throws InferenceError if the
    // chat template drops or repeats a content.
```

In `src/llama/LlamaCppBackend.cpp`:
- delete the constant and its comment:

```cpp
// The first candidate for the marker that stands in for message contents while the chat template renders
// (TokenizePrompt): ASCII record separator, a byte that ordinary text does not use.
constexpr char CONTENT_MARKER_BYTE = '\x1E';

```

- in the constructor's thinking block, replace

```cpp
        const std::vector<std::int32_t> end = Tokenize(pVocab, "</think>", false, true);
```

  with

```cpp
        const std::vector<std::int32_t> end = Tokenize(pVocab, inference::THINKING_END_TEXT, false, true);
```

  and replace

```cpp
        markers.open = Tokenize(pVocab, "<think>\n", false, true);
        markers.skip = Tokenize(pVocab, "<think>\n\n</think>\n\n", false, true);
        markers.close = Tokenize(pVocab, CLOSE_THINKING_TEXT, false, true);
        markers.afterEnd = Tokenize(pVocab, "\n\n", false, false);
```

  with

```cpp
        markers.open = Tokenize(pVocab, inference::THINKING_OPEN_TEXT, false, true);
        markers.skip = Tokenize(pVocab, inference::THINKING_SKIP_TEXT, false, true);
        markers.close = Tokenize(pVocab, inference::CLOSE_THINKING_TEXT, false, true);
        markers.afterEnd = Tokenize(pVocab, inference::THINKING_AFTER_END_TEXT, false, false);
```

- replace the whole body of `LlamaCppBackend::TokenizePrompt` (from its opening brace to its closing brace) with:

```cpp
{
    // Message contents are text, never control tokens: the template is rendered with a marker in place of each
    // content, and the pieces are tokenized apart: template text with parse_special (its control tokens are real),
    // contents without it.
    const std::string marker = inference::ContentMarker(prompt);
    const std::vector<std::string> pieces =
        inference::TemplatePieces(RenderPrompt(inference::MarkContents(prompt, marker)), marker, prompt.messages.size());

    const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());
    // BOS is added when the model expects one; if the template already wrote it, the duplicate is dropped below.
    const bool addBos = llama_vocab_get_add_bos(pVocab);
    std::vector<std::int32_t> tokens;
    auto append = [&tokens](const std::vector<std::int32_t>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
        // The template's text before content `piece` (after the last content, for the last piece).
        if (!pieces[piece].empty() || (piece == 0 && addBos)) {
            append(Tokenize(pVocab, pieces[piece], piece == 0 && addBos, true));
        }
        if (piece < prompt.messages.size() && !prompt.messages[piece].content.empty()) {
            append(Tokenize(pVocab, prompt.messages[piece].content, false, false));
        }
    }
    if (addBos && tokens.size() >= 2 && tokens[0] == llama_vocab_bos(pVocab) && tokens[1] == tokens[0]) {
        tokens.erase(tokens.begin());
    }
    // A thinking model's reply starts with its thinking block: empty here (thinking off), as its template writes it.
    if (_thinking) {
        tokens.insert(tokens.end(), _thinking->skip.begin(), _thinking->skip.end());
    }
    MORPHEUS_REQUIRE(!tokens.empty(), "TokenizePrompt: the rendered prompt has no tokens");
    return tokens;
}
```

(The behaviour is unchanged: the same marker, the same pieces, the same error texts; the `[llama]` tests confirm it in Step 6. If `<algorithm>` is no longer used in `LlamaCppBackend.cpp`, keep it anyway: `std::min` still is.)

- [ ] **Step 4: Write the backend**

Create `src/llamaserver/LlamaServerBackend.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

namespace morpheus::llamaserver {

// Where the models are and what Morpheus plans for. The callers build it from core::Settings.
struct LlamaServerConfig {
    std::string chatUrl = {};                 // "http://host:port" of the chat model's llama-server.
    std::string embeddingUrl = {};            // "http://host:port" of the embedding model's llama-server (--embedding).
    int slot = 0;                             // The server slot of calls whose GenerationSpec::slot is -1.
    int contextSize = 0;                      // settings.json's context_size: ContextSize(), what the budgets use.
    std::string embeddingQueryPrefix = {};    // Prepended for EmbedPurpose::Query.
    std::string embeddingDocumentPrefix = {}; // Prepended for EmbedPurpose::Document.
};

// The models through llama-server over HTTP (cpp-httplib), so one model host can serve several consumers. Token
// counts come from the chat server's POST /tokenize; prompts are rendered by its POST /apply-template around content
// markers and tokenized piece by piece, contents as plain text, exactly as LlamaCppBackend does, and sent to POST
// /completion as token ids on a slot with cache_prompt; vectors come from the embedding server's POST /embedding.
//
// Thinking models (the chat template has the enable_thinking switch): as LlamaCppBackend, the backend writes the
// thinking block's markers itself. A call that thinks is two /completion calls on one slot: the block, which stops at
// "</think>" or at its budget, then the answer, whose prompt extends the first one's, so the server reuses its cache.
//
// Not thread-safe: one thread owns it. Several backends may talk to one server at once, each on its own slot.
class LlamaServerBackend final : public inference::LlmBackend {
private:
    // The thinking block's texts as the chat server tokenizes them, as LlamaCppBackend::ThinkingMarkers.
    struct ThinkingMarkers {
        std::vector<std::int32_t> open = {};     // inference::THINKING_OPEN_TEXT, special tokens parsed.
        std::vector<std::int32_t> skip = {};     // inference::THINKING_SKIP_TEXT, special tokens parsed.
        std::vector<std::int32_t> close = {};    // inference::CLOSE_THINKING_TEXT, special tokens parsed.
        std::vector<std::int32_t> end = {};      // inference::THINKING_END_TEXT, special tokens parsed.
        std::vector<std::int32_t> afterEnd = {}; // inference::THINKING_AFTER_END_TEXT, as plain text.
    };

    LlamaServerConfig _config = {};
    std::optional<ThinkingMarkers> _thinking = {}; // Set when the chat template has the enable_thinking switch.
    std::optional<std::int32_t> _bos = {};         // The BOS token the chat model adds at a prompt's start, if any.
    std::string _embeddingModelId = {};            // The embedding server's model file name, without its extension.

public:
    // Reads both servers' GET /props: the chat server's context per slot, which must hold config.contextSize, and its
    // chat template (whether the model thinks); the embedding server's model file. Then tokenizes the thinking texts
    // (thinking models) and an empty text with add_special (the BOS the model adds, if any). Throws InferenceError
    // naming the URL for a URL that is not http://host:port, an unreachable server, an HTTP error, an unexpected
    // answer, or slots smaller than config.contextSize. Throws ContractError for a context size below 1 or a slot
    // below 0.
    explicit LlamaServerBackend(LlamaServerConfig config);

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    // Not supported: llama-server keeps its slots' caches itself. Both throw InferenceError.
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;

private:
    // The prompt's tokens as Complete sends them without thinking: the chat server's template rendered around content
    // markers (inference::ContentMarker), each template piece tokenized with special tokens parsed (the first with
    // BOS when the model adds one, never twice), each content as plain text, then a thinking model's empty block.
    // Throws InferenceError if the chat template drops or repeats a content.
    std::vector<std::int32_t> TokenizePrompt(const inference::Prompt& prompt) const;

    // The chat server's tokens for `text`: with addSpecial, BOS where the model wants one; with parseSpecial,
    // control-token text becomes control tokens.
    std::vector<std::int32_t> ServerTokens(std::string_view text, bool addSpecial, bool parseSpecial) const;
};

} // namespace morpheus::llamaserver
```

Create `src/llamaserver/LlamaServerBackend.cpp`:

```cpp
#include "llamaserver/LlamaServerBackend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

#include "core/Errors.h"

namespace morpheus::llamaserver {

using inference::InferenceError;

namespace {

// How long a request may take to connect, and to wait for the server's next bytes. A long prompt is read before the
// first piece of its answer arrives, so the read wait is generous.
constexpr int CONNECT_TIMEOUT_SECONDS = 5;
constexpr int READ_TIMEOUT_SECONDS = 600;
// How much of a server's answer an error message quotes, in bytes.
constexpr std::size_t QUOTED_ANSWER_BYTES = 300;
// A server URL is this, then a host with an optional port: no HTTPS and no path.
constexpr std::string_view URL_SCHEME = "http://";

// Gives `client` Morpheus's timeouts.
void SetTimeouts(httplib::Client& client)
{
    client.set_connection_timeout(CONNECT_TIMEOUT_SECONDS, 0);
    client.set_read_timeout(READ_TIMEOUT_SECONDS, 0);
}

// The start of a server's answer, for an error message.
std::string Quoted(std::string_view answer)
{
    return std::string(answer.substr(0, QUOTED_ANSWER_BYTES));
}

// `body` as JSON text. JSON needs UTF-8, so a text that is not UTF-8 cannot be sent.
std::string BodyText(const nlohmann::json& body)
{
    try {
        return body.dump();
    } catch (const nlohmann::json::type_error& error) {
        throw InferenceError(std::format("a text for llama-server is not UTF-8: {}", error.what()));
    }
}

// Sends GET `path` (no body) or POST `path` with a JSON body to `url`, and reads the JSON answer with `read`.
// Throws InferenceError naming the URL and path for an unreachable server, an HTTP error, an answer that is not JSON,
// or one `read` cannot read (a missing field, a wrong type).
template <typename Read>
auto Ask(const std::string& url, const std::string& path, const std::optional<nlohmann::json>& body, Read&& read)
{
    httplib::Client client(url);
    SetTimeouts(client);
    const httplib::Result response = body ? client.Post(path, BodyText(*body), "application/json") : client.Get(path);
    if (!response) {
        throw InferenceError(std::format("cannot reach {}{}: {}", url, path, httplib::to_string(response.error())));
    }
    if (response->status != 200) {
        throw InferenceError(std::format("{}{} answered HTTP {}: {}", url, path, response->status, Quoted(response->body)));
    }
    try {
        return read(nlohmann::json::parse(response->body));
    } catch (const nlohmann::json::exception& error) {
        throw InferenceError(
            std::format("{}{} gave an unexpected answer ({}): {}", url, path, error.what(), Quoted(response->body)));
    }
}

} // namespace

LlamaServerBackend::LlamaServerBackend(LlamaServerConfig config)
    : _config(std::move(config))
{
    MORPHEUS_REQUIRE(_config.contextSize > 0, "LlamaServerBackend: the context size must be positive");
    MORPHEUS_REQUIRE(_config.slot >= 0, "LlamaServerBackend: the slot must be at least 0");

    // Each URL is "http://" and a host with an optional port. cpp-httplib would quietly drop a path, and HTTPS is
    // not supported.
    for (const std::string* pUrl : {&_config.chatUrl, &_config.embeddingUrl}) {
        const std::string_view host = std::string_view(*pUrl).substr(std::min(pUrl->size(), URL_SCHEME.size()));
        if (!pUrl->starts_with(URL_SCHEME) || host.empty() || host.find_first_of("/?#") != std::string_view::npos) {
            throw InferenceError(
                std::format("'{}' is not a llama-server URL: it must be http://host:port, without a path", *pUrl));
        }
    }

    // The chat server: each slot must hold the context the budgets plan for, and its template tells whether the
    // model thinks.
    const auto [slotContext, chatTemplate] = Ask(_config.chatUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
        return std::pair(props.at("default_generation_settings").at("n_ctx").get<int>(),
                         props.at("chat_template").get<std::string>());
    });
    if (slotContext < _config.contextSize) {
        throw InferenceError(std::format("the chat server at {} holds {} tokens per slot, less than the context size "
                                         "{} (start llama-server with a --ctx-size of at least the context size times "
                                         "--parallel)",
                                         _config.chatUrl, slotContext, _config.contextSize));
    }
    if (chatTemplate.find("enable_thinking") != std::string::npos) {
        // The thinking block's texts, tokenized once as the in-process backend tokenizes them: the markers with
        // special tokens parsed, the blank line after the end marker as plain text.
        ThinkingMarkers markers;
        markers.open = ServerTokens(inference::THINKING_OPEN_TEXT, false, true);
        markers.skip = ServerTokens(inference::THINKING_SKIP_TEXT, false, true);
        markers.close = ServerTokens(inference::CLOSE_THINKING_TEXT, false, true);
        markers.end = ServerTokens(inference::THINKING_END_TEXT, false, true);
        markers.afterEnd = ServerTokens(inference::THINKING_AFTER_END_TEXT, false, false);
        _thinking = std::move(markers);
    }
    // The BOS the model adds, if any: what add_special puts in front of an empty text.
    if (const std::vector<std::int32_t> special = ServerTokens("", true, false); !special.empty()) {
        _bos = special.front();
    }

    // The embedding server: its model file's name, without the extension, names the vectors it makes, as the
    // in-process backend names them, so a run's stored vectors stay valid when the backend changes.
    const std::string modelPath = Ask(_config.embeddingUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
        return props.at("model_path").get<std::string>();
    });
    const std::size_t nameStart = modelPath.find_last_of("/\\");
    const std::string fileName = modelPath.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    const std::size_t dot = fileName.rfind('.');
    _embeddingModelId = dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
    if (_embeddingModelId.empty()) {
        throw InferenceError(std::format("the embedding server at {} names no model file in /props (model_path '{}')",
                                         _config.embeddingUrl, modelPath));
    }
}

int LlamaServerBackend::CountTokens(std::string_view text)
{
    return static_cast<int>(ServerTokens(text, false, false).size());
}

int LlamaServerBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return static_cast<int>(TokenizePrompt(prompt).size());
}

int LlamaServerBackend::ContextSize() const
{
    return _config.contextSize;
}

inference::Completion LlamaServerBackend::Complete(const inference::Prompt& /*prompt*/,
                                                   const inference::GenerationSpec& /*spec*/,
                                                   const inference::TokenCallback& /*onToken*/, std::stop_token /*stop*/)
{
    throw InferenceError("LlamaServerBackend::Complete is not implemented yet");
}

std::vector<std::vector<float>> LlamaServerBackend::Embed(const std::vector<std::string>& texts,
                                                          inference::EmbedPurpose purpose)
{
    const std::string& prefix = purpose == inference::EmbedPurpose::Query ? _config.embeddingQueryPrefix
                                                                          : _config.embeddingDocumentPrefix;
    std::vector<std::vector<float>> vectors;
    vectors.reserve(texts.size());
    for (const std::string& text : texts) {
        // One result for the one text, holding one vector: a pooled sentence-embedding model. A model without
        // pooling answers a vector per token.
        std::vector<float> vector = Ask(_config.embeddingUrl, "/embedding", nlohmann::json{{"content", prefix + text}},
                                        [this](const nlohmann::json& answer) {
                                            const std::size_t vectorCount =
                                                answer.size() == 1 ? answer.at(0).at("embedding").size() : 0;
                                            if (answer.size() != 1 || vectorCount != 1) {
                                                throw InferenceError(std::format(
                                                    "{}/embedding answered {} results and {} vectors for one text; it "
                                                    "needs one pooled vector (a sentence-embedding model)",
                                                    _config.embeddingUrl, answer.size(), vectorCount));
                                            }
                                            return answer.at(0).at("embedding").at(0).get<std::vector<float>>();
                                        });

        // L2-normalize, so cosine similarity is a dot product.
        double norm = 0.0;
        for (const float value : vector) {
            norm += static_cast<double>(value) * value;
        }
        norm = std::sqrt(norm);
        if (norm == 0.0) {
            throw InferenceError(std::format("{}/embedding returned a zero vector", _config.embeddingUrl));
        }
        for (float& value : vector) {
            value = static_cast<float>(value / norm);
        }
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

std::string LlamaServerBackend::EmbeddingModelId() const
{
    return _embeddingModelId;
}

std::vector<std::uint8_t> LlamaServerBackend::SaveState()
{
    throw InferenceError("saving the chat cache is not supported by the llama-server backend");
}

void LlamaServerBackend::RestoreState(std::span<const std::uint8_t> /*state*/)
{
    throw InferenceError("restoring the chat cache is not supported by the llama-server backend");
}

std::vector<std::int32_t> LlamaServerBackend::TokenizePrompt(const inference::Prompt& prompt) const
{
    MORPHEUS_REQUIRE(!prompt.messages.empty(), "TokenizePrompt: the prompt has no messages");

    // The server's template renders the prompt with a marker in place of each content. add_generation_prompt: the
    // text ends where the assistant's reply begins. A thinking model's template renders with its switch on, so it
    // writes no empty block of its own: the markers are the backend's, as in-process.
    const std::string marker = inference::ContentMarker(prompt);
    nlohmann::json messages = nlohmann::json::array();
    for (const inference::Message& message : inference::MarkContents(prompt, marker).messages) {
        messages.push_back(nlohmann::json{{"role", inference::RoleName(message.role)}, {"content", message.content}});
    }
    nlohmann::json body = {{"messages", messages}, {"add_generation_prompt", true}};
    if (_thinking) {
        body["chat_template_kwargs"] = {{"enable_thinking", true}};
    }
    std::string rendered = Ask(_config.chatUrl, "/apply-template", body, [](const nlohmann::json& answer) {
        return answer.at("prompt").get<std::string>();
    });
    // A template that opens the block itself when thinking is on ends with the open marker; the markers come from the
    // backend, so it goes.
    if (_thinking && rendered.ends_with(inference::THINKING_OPEN_TEXT)) {
        rendered.resize(rendered.size() - inference::THINKING_OPEN_TEXT.size());
    }
    const std::vector<std::string> pieces = inference::TemplatePieces(rendered, marker, prompt.messages.size());

    // The pieces apart: template text with special tokens parsed (its control tokens are real; BOS on the first piece
    // when the model adds one), contents as plain text, whatever they hold.
    std::vector<std::int32_t> tokens;
    auto append = [&tokens](const std::vector<std::int32_t>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
        const bool addBos = piece == 0 && _bos.has_value();
        if (!pieces[piece].empty() || addBos) {
            append(ServerTokens(pieces[piece], addBos, true));
        }
        if (piece < prompt.messages.size() && !prompt.messages[piece].content.empty()) {
            append(ServerTokens(prompt.messages[piece].content, false, false));
        }
    }
    // A template that writes BOS itself gets no second one, as in-process.
    if (_bos && tokens.size() >= 2 && tokens[0] == *_bos && tokens[1] == *_bos) {
        tokens.erase(tokens.begin());
    }
    // A thinking model's reply starts with its thinking block: empty here (thinking off), as its template writes it.
    if (_thinking) {
        append(_thinking->skip);
    }
    MORPHEUS_REQUIRE(!tokens.empty(), "TokenizePrompt: the rendered prompt has no tokens");
    return tokens;
}

std::vector<std::int32_t> LlamaServerBackend::ServerTokens(std::string_view text, bool addSpecial, bool parseSpecial) const
{
    const nlohmann::json body = {{"content", std::string(text)}, {"add_special", addSpecial}, {"parse_special", parseSpecial}};
    return Ask(_config.chatUrl, "/tokenize", body, [](const nlohmann::json& answer) {
        return answer.at("tokens").get<std::vector<std::int32_t>>();
    });
}

} // namespace morpheus::llamaserver
```

Create `src/llamaserver/CMakeLists.txt`:

```cmake
add_library(morpheus_llamaserver STATIC
    LlamaServerBackend.cpp
)
add_library(Morpheus::llamaserver ALIAS morpheus_llamaserver) # The name host projects link.
target_include_directories(morpheus_llamaserver PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_llamaserver
    PUBLIC morpheus_inference
    PRIVATE morpheus_core nlohmann_json::nlohmann_json morpheus_httplib
)
morpheus_target_defaults(morpheus_llamaserver)
```

In the root `CMakeLists.txt`, after `add_subdirectory(src/turn)`, add:

```cmake
if(MORPHEUS_BUILD_LLAMA_SERVER)
    add_subdirectory(src/llamaserver)
endif()
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[inference]"`
Expected: builds with no warnings; `All tests passed` (10 `[llamaserver]` test cases, and `[inference]` with the 2 new ones). The "unreachable" sections take up to a few seconds each (Windows retries a refused local connection).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 389`.

- [ ] **Step 6: Build the dev targets and run the model-backed tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings (`LlamaCppBackend` now takes its markers and its content markers from `inference`).

Run, with the two model variables exported as in Task 2: `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"`
Expected: `All tests passed`: the completion and hardening tests (contents holding `\x1E` and control-token text) show the rewritten `TokenizePrompt` behaves as before. The thinking tests skip unless `MORPHEUS_TEST_THINKING_MODEL` is also set (set it to a Qwen3.5 GGUF in `models/` if one is there, to check the in-process thinking block is unchanged).

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes src/inference/Llm.h src/inference/Llm.cpp src/llama/LlamaCppBackend.h src/llama/LlamaCppBackend.cpp src/llamaserver/LlamaServerBackend.h src/llamaserver/LlamaServerBackend.cpp src/llamaserver/CMakeLists.txt CMakeLists.txt tests/CMakeLists.txt tests/inference/LlmTests.cpp tests/llamaserver/ServerTestSupport.h tests/llamaserver/LlamaServerBackendTests.cpp
git add src/inference/Llm.h src/inference/Llm.cpp src/llama/LlamaCppBackend.h src/llama/LlamaCppBackend.cpp src/llamaserver/LlamaServerBackend.h src/llamaserver/LlamaServerBackend.cpp src/llamaserver/CMakeLists.txt CMakeLists.txt tests/CMakeLists.txt tests/inference/LlmTests.cpp tests/llamaserver/ServerTestSupport.h tests/llamaserver/LlamaServerBackendTests.cpp
git commit -m "Add morpheus_llamaserver: startup, prompt tokens with contents as plain text, embeddings; share the content markers" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Expected: `check_bytes` prints nothing (the `\x1E`, `\n` and `\u001e` must stay escapes).

---

### Task 6: LlamaServerBackend::Complete: streaming, thinking, slots, telemetry, cancel

`Complete` replaces the stub: one streamed `/completion` call, or two when the model thinks. The prompt goes as token ids (Task 5's `TokenizePrompt`), so a content never becomes control tokens; the request body carries the slot, `cache_prompt`, and the sampling of `LlamaCppBackend`'s chain with every other llama-server sampler neutral; the server's `timings` fill the telemetry; a stop closes the connection at once.

**Files:**
- Modify: `src/llamaserver/LlamaServerBackend.cpp`
- Test: `tests/llamaserver/LlamaServerCompleteTests.cpp`

**Interfaces:**
- Consumes: everything of Task 5; `test::FakeLlamaServer::CompletionScript` (Task 4); `inference::THINKING_*` and `CLOSE_THINKING_TEXT` (Task 5).
- Produces: `LlamaServerBackend::Complete` as `ChatModel::Complete` documents it. The body of each `/completion` request: `prompt` (an array of token ids), `n_predict`, the sampling (`samplers: ["top_k", "top_p", "temperature"]`, `top_k`, `top_p`, `min_keep: 1`, `temperature`, `dynatemp_range: 0`, `dynatemp_exponent: 1`, `seed`, and neutral `mirostat: 0`, `min_p: 0`, `typical_p: 1`, `top_n_sigma: -1`, `xtc_probability: 0`, `repeat_penalty: 1`, `repeat_last_n: 0`, `frequency_penalty: 0`, `presence_penalty: 0`, `dry_multiplier: 0`, `logit_bias: []`, `ignore_eos: false`, `n_probs: 0`), `id_slot` (`spec.slot`, or the configured slot when -1), `cache_prompt: true`, `stream: true`; `grammar` on the answer call when set; on the thinking call `stop: ["</think>"]` and `n_predict` = `thinkingMaxTokens` minus the closing sentence's tokens. The answer call after thinking sends the thinking call's prompt tokens, then the thinking text tokenized as plain text (`/tokenize`, `add_special` and `parse_special` false), then the end marker and blank line, or the closing sentence, as tokens. Errors: `ContractError` for `maxTokens <= 0`, a negative `thinkingMaxTokens`, one too small to close the block, a slot below -1; `InferenceError` for a prompt plus generation over `ContextSize()`, thinking asked of a model without a thinking mode, an HTTP error, a malformed stream, an unknown `stop_type`, a stream that ends early, and a server prompt count that differs from `/tokenize`'s.

- [ ] **Step 1: Write the failing tests**

Create `tests/llamaserver/LlamaServerCompleteTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "fakes/FakeLlamaServer.h"
#include "inference/Llm.h"
#include "llamaserver/LlamaServerBackend.h"
#include "llamaserver/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string YES_NO = "root ::= \"yes\" | \"no\"\n";

// `text`'s tokens by the fake's default tokenizer, as an array of ids.
std::vector<int> Tokens(std::string_view text)
{
    return test::FakeLlamaServer::DefaultTokens(std::string(text), false, false);
}

// The ids of `parts`, one after the other.
std::vector<int> Joined(std::initializer_list<std::vector<int>> parts)
{
    std::vector<int> tokens;
    for (const std::vector<int>& part : parts) {
        tokens.insert(tokens.end(), part.begin(), part.end());
    }
    return tokens;
}

} // namespace

TEST_CASE("LlamaServerBackend::Complete sends one streamed /completion request", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    chat.QueueCompletion(script);
    chat.QueueCompletion(script);

    inference::GenerationSpec spec;
    spec.grammar = YES_NO;
    spec.maxTokens = 16;
    spec.sampling = {0.5f, 20, 0.75f, 77};
    backend.Complete(test::SERVER_PROMPT, spec, {}, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 1);
    const nlohmann::json& body = requests[0].body;
    // The prompt as token ids: no content is ever parsed for control tokens.
    CHECK(body.at("prompt") == nlohmann::json(test::ServerPromptTokens()));
    CHECK(body.at("n_predict") == 16);
    CHECK(body.at("grammar") == YES_NO);
    // The sampling of LlamaCppBackend's chain: top-k, top-p keeping at least one token, temperature, then the draw.
    CHECK(body.at("samplers") == nlohmann::json::array({"top_k", "top_p", "temperature"}));
    CHECK(body.at("top_k") == 20);
    CHECK(body.at("top_p").get<double>() == 0.75);
    CHECK(body.at("min_keep") == 1);
    CHECK(body.at("temperature").get<double>() == 0.5);
    CHECK(body.at("dynatemp_range").get<double>() == 0.0);
    CHECK(body.at("dynatemp_exponent").get<double>() == 1.0);
    CHECK(body.at("seed") == 77);
    // Every other sampler llama-server knows is neutral, whatever its command line set.
    CHECK(body.at("mirostat") == 0);
    CHECK(body.at("min_p").get<double>() == 0.0);
    CHECK(body.at("typical_p").get<double>() == 1.0);
    CHECK(body.at("top_n_sigma").get<double>() == -1.0);
    CHECK(body.at("xtc_probability").get<double>() == 0.0);
    CHECK(body.at("repeat_penalty").get<double>() == 1.0);
    CHECK(body.at("repeat_last_n") == 0);
    CHECK(body.at("frequency_penalty").get<double>() == 0.0);
    CHECK(body.at("presence_penalty").get<double>() == 0.0);
    CHECK(body.at("dry_multiplier").get<double>() == 0.0);
    CHECK(body.at("logit_bias") == nlohmann::json::array());
    CHECK(body.at("ignore_eos") == false);
    CHECK(body.at("n_probs") == 0);
    CHECK(body.at("id_slot") == 1); // spec.slot -1: the configured slot.
    CHECK(body.at("cache_prompt") == true);
    CHECK(body.at("stream") == true);
    CHECK_FALSE(body.contains("stop"));

    // Without a grammar, none is sent.
    spec.grammar.clear();
    backend.Complete(test::SERVER_PROMPT, spec, {}, {});
    CHECK_FALSE(chat.RequestsTo("/completion").back().body.contains("grammar"));
}

TEST_CASE("LlamaServerBackend::Complete sends an explicit slot as id_slot", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    chat.QueueCompletion(script);

    inference::GenerationSpec spec;
    spec.slot = 2;
    backend.Complete(test::SERVER_PROMPT, spec, {}, {});
    CHECK(chat.RequestsTo("/completion").back().body.at("id_slot") == 2);
}

TEST_CASE("LlamaServerBackend::Complete streams the pieces in order and fills the telemetry from timings", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"The ", "gate ", "opens."};
    script.promptN = 4; // The slot's cache held all but 4 of the prompt's tokens.
    script.promptMs = 12.5;
    script.predictedMs = 40.0;
    chat.QueueCompletion(script);

    std::vector<std::string> pieces;
    const inference::Completion completion = backend.Complete(
        test::SERVER_PROMPT, inference::GenerationSpec{}, [&](std::string_view piece) { pieces.emplace_back(piece); }, {});
    CHECK(pieces == std::vector<std::string>{"The ", "gate ", "opens."});
    CHECK(completion.text == "The gate opens.");
    const int promptTokens = static_cast<int>(test::ServerPromptTokens().size());
    CHECK(completion.promptTokens == promptTokens);
    CHECK(completion.tokensEvaluated == 4);
    CHECK(completion.tokensReused == promptTokens - 4);
    CHECK(completion.tokensGenerated == 3);
    CHECK(completion.promptMs == 12.5);
    CHECK(completion.generateMs == 40.0);
    CHECK_FALSE(completion.stoppedAtLimit);
    CHECK_FALSE(completion.cancelled);
    CHECK(completion.thinking.empty());
    CHECK(completion.thinkingTokens == 0);
}

TEST_CASE("LlamaServerBackend::Complete reports an answer stopped at its token limit", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"The gate"};
    script.stopType = "limit";
    chat.QueueCompletion(script);
    CHECK(backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}).stoppedAtLimit);
}

TEST_CASE("LlamaServerBackend::Complete fails on an HTTP error or a stream it cannot read", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"a"};
    auto complete = [&] { return backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}); };

    SECTION("an HTTP error")
    {
        chat.SetStatus("/completion", 500);
        CHECK_THROWS_AS(complete(), inference::InferenceError);
        CHECK_THROWS_WITH(complete(), ContainsSubstring(chat.Url() + "/completion") && ContainsSubstring("500"));
    }
    SECTION("a line that is not a message")
    {
        script.extraLine = "event: nonsense";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("event: nonsense"));
    }
    SECTION("a message that is not JSON")
    {
        script.extraLine = "data: {not json";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("{not json"));
    }
    SECTION("a message without its fields")
    {
        script.extraLine = R"(data: {"stop":false})";
        chat.QueueCompletion(script);
        CHECK_THROWS_AS(complete(), inference::InferenceError);
    }
    SECTION("an error in the stream")
    {
        script.extraLine = R"(data: {"error":{"code":500,"message":"out of memory","type":"server_error"}})";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("out of memory"));
    }
    SECTION("a stream that ends before its last message")
    {
        script.endEarly = true;
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("ended before its last message"));
    }
    SECTION("an unknown stop_type")
    {
        script.stopType = "tired";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("tired"));
    }
}

TEST_CASE("LlamaServerBackend::Complete checks its contract like the in-process backend", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    inference::GenerationSpec spec;

    spec.maxTokens = 0;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
    spec.maxTokens = 16;
    spec.thinkingMaxTokens = -1;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
    spec.thinkingMaxTokens = 0;
    spec.slot = -2;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
    spec.slot = -1;

    // The prompt plus the answer over the context size (8192): refused before anything is generated.
    spec.maxTokens = 8192;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), inference::InferenceError);
    CHECK_THROWS_WITH(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), ContainsSubstring("exceeds the context size (8192)"));
    CHECK(chat.RequestsTo("/completion").empty());

    // The server must count the prompt as /tokenize does: the budgets rely on that count.
    spec.maxTokens = 16;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    script.tokensEvaluated = 999;
    chat.QueueCompletion(script);
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), inference::InferenceError);
    chat.QueueCompletion(script);
    CHECK_THROWS_WITH(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), ContainsSubstring("999"));
}

TEST_CASE("LlamaServerBackend::Complete thinks in a first call, then answers in a second that extends it", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    // The model closes its block: llama-server stops at the stop word "</think>" and leaves it out of the text.
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"The gate ", "is shut."};
    thought.stopType = "word";
    thought.promptN = 5;
    thought.promptMs = 10.0;
    thought.predictedN = 4;
    thought.predictedMs = 30.0;
    chat.QueueCompletion(thought);
    test::FakeLlamaServer::CompletionScript answer;
    answer.pieces = {"no"};
    answer.promptMs = 2.0;
    answer.predictedMs = 8.0;
    chat.QueueCompletion(answer);

    inference::GenerationSpec spec;
    spec.grammar = YES_NO;
    spec.maxTokens = 16;
    spec.thinkingMaxTokens = 256;
    std::string streamed;
    const inference::Completion completion =
        backend.Complete(test::SERVER_PROMPT, spec, [&](std::string_view piece) { streamed += piece; }, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 2);
    // The prompt's tokens end with the open block (the marker tokenized with special tokens parsed).
    const std::vector<int> opened = Joined({test::ServerPromptTokens(), Tokens(inference::THINKING_OPEN_TEXT)});
    // The thinking call: the open block, no grammar, its budget less the closing sentence, stopping at the end marker.
    CHECK(requests[0].body.at("prompt") == nlohmann::json(opened));
    CHECK_FALSE(requests[0].body.contains("grammar"));
    CHECK(requests[0].body.at("n_predict") == 256 - static_cast<int>(Tokens(inference::CLOSE_THINKING_TEXT).size()));
    CHECK(requests[0].body.at("stop") == nlohmann::json::array({"</think>"}));
    // The answer call continues the same tokens on the same slot: the thinking text as plain text, the end marker and
    // the blank line, then the answer under the grammar.
    CHECK(requests[1].body.at("prompt") == nlohmann::json(Joined({opened, Tokens("The gate is shut."),
                                                                 Tokens(inference::THINKING_END_TEXT),
                                                                 Tokens(inference::THINKING_AFTER_END_TEXT)})));
    const std::vector<test::FakeLlamaServer::Request> tokenized = chat.RequestsTo("/tokenize");
    CHECK(std::any_of(tokenized.begin(), tokenized.end(), [](const test::FakeLlamaServer::Request& request) {
        return request.body == nlohmann::json{{"content", "The gate is shut."}, {"add_special", false}, {"parse_special", false}};
    }));
    CHECK(requests[1].body.at("grammar") == YES_NO);
    CHECK(requests[1].body.at("n_predict") == 16);
    CHECK_FALSE(requests[1].body.contains("stop"));
    CHECK(requests[1].body.at("id_slot") == requests[0].body.at("id_slot"));

    // The thinking stays out of the text and the stream.
    CHECK(completion.text == "no");
    CHECK(streamed == "no");
    CHECK(completion.thinking == "The gate is shut.");
    // The prompt's numbers come from the first call; the block's tokens are what the server generated plus the blank
    // line after the end marker; its time covers the answer call's reading of it.
    const int promptTokens = static_cast<int>(opened.size());
    CHECK(completion.promptTokens == promptTokens);
    CHECK(completion.tokensEvaluated == 5);
    CHECK(completion.tokensReused == promptTokens - 5);
    CHECK(completion.promptMs == 10.0);
    CHECK(completion.thinkingTokens == 4 + static_cast<int>(Tokens(inference::THINKING_AFTER_END_TEXT).size()));
    CHECK(completion.thinkingMs == 30.0 + 2.0);
    CHECK_FALSE(completion.thinkingStoppedAtLimit);
    CHECK(completion.tokensGenerated == 1);
    CHECK(completion.generateMs == 8.0);
}

TEST_CASE("LlamaServerBackend::Complete closes a thinking block that reaches its budget", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    const int closeTokens = static_cast<int>(Tokens(inference::CLOSE_THINKING_TEXT).size());
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"Hmm, "};
    thought.stopType = "limit";
    thought.predictedN = 256 - closeTokens;
    chat.QueueCompletion(thought);
    test::FakeLlamaServer::CompletionScript answer;
    answer.pieces = {"yes"};
    chat.QueueCompletion(answer);

    inference::GenerationSpec spec;
    spec.grammar = YES_NO;
    spec.thinkingMaxTokens = 256;
    const inference::Completion completion = backend.Complete(test::SERVER_PROMPT, spec, {}, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].body.at("prompt") == nlohmann::json(Joined({test::ServerPromptTokens(), Tokens(inference::THINKING_OPEN_TEXT),
                                                                 Tokens("Hmm, "), Tokens(inference::CLOSE_THINKING_TEXT)})));
    CHECK(completion.thinkingStoppedAtLimit);
    CHECK(completion.thinkingTokens == 256);
    CHECK(completion.thinking == "Hmm, ");
    CHECK(completion.text == "yes");
}

TEST_CASE("LlamaServerBackend::Complete gives no answer when the model ends its reply while thinking", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"Nothing to say."};
    thought.stopType = "eos";
    chat.QueueCompletion(thought);

    inference::GenerationSpec spec;
    spec.thinkingMaxTokens = 256;
    const inference::Completion completion = backend.Complete(test::SERVER_PROMPT, spec, {}, {});
    CHECK(chat.RequestsTo("/completion").size() == 1);
    CHECK(completion.thinking == "Nothing to say.");
    CHECK(completion.text.empty());
    CHECK_FALSE(completion.cancelled);
}

TEST_CASE("LlamaServerBackend::Complete refuses thinking it cannot do", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    inference::GenerationSpec spec;
    spec.thinkingMaxTokens = 256;

    // A model without a thinking mode: an InferenceError, as in-process, before anything is generated.
    llamaserver::LlamaServerBackend plain(test::ConfigFor(chat, embed));
    CHECK_THROWS_AS(plain.Complete(test::SERVER_PROMPT, spec, {}, {}), inference::InferenceError);
    CHECK_THROWS_WITH(plain.Complete(test::SERVER_PROMPT, spec, {}, {}), ContainsSubstring("no thinking mode"));
    CHECK(chat.RequestsTo("/completion").empty());

    // A budget that cannot hold the closing sentence: an engine bug.
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend thinking(test::ConfigFor(chat, embed));
    spec.thinkingMaxTokens = static_cast<int>(Tokens(inference::CLOSE_THINKING_TEXT).size());
    CHECK_THROWS_AS(thinking.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
}

TEST_CASE("A stop ends LlamaServerBackend::Complete mid-stream and closes the connection", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"The ", "gate"};
    script.holdOpen = true; // The stream stays open after the pieces, as a server still generating.
    chat.QueueCompletion(script);

    // The first piece asks for the stop.
    std::stop_source source;
    std::string streamed;
    const inference::Completion completion = backend.Complete(
        test::SERVER_PROMPT, inference::GenerationSpec{},
        [&](std::string_view piece) {
            streamed += piece;
            source.request_stop();
        },
        source.get_token());
    CHECK(completion.cancelled);
    CHECK(streamed == "The ");

    // llama-server cancels a task whose connection closed: the fake sees the close on one of its next pings.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!chat.ClientClosedStream() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(chat.ClientClosedStream());

    // A stop requested before the call sends no completion request.
    const inference::Completion early = backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, source.get_token());
    CHECK(early.cancelled);
    CHECK(chat.RequestsTo("/completion").size() == 1);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "LlamaServerBackend::Complete*" "A stop ends LlamaServerBackend*"`
Expected: FAIL: most cases throw `InferenceError` "LlamaServerBackend::Complete is not implemented yet" (the contract test's first `CHECK_THROWS_AS(..., core::ContractError)` fails the same way).

- [ ] **Step 3: Write Complete**

In `src/llamaserver/LlamaServerBackend.cpp`, add `#include <exception>`, `#include <functional>` and `#include <stop_token>` to the standard includes, keeping them sorted.

In the anonymous namespace, after the `Ask` template, add:

```cpp

// What one streamed /completion call returned.
struct StreamResult {
    bool cancelled = false;    // A stop ended it before its last message.
    std::string stopType = {}; // Why generation stopped: "eos", "limit" or "word" (a stop string).
    int promptTokens = 0;      // tokens_evaluated: the prompt's tokens, as the server counted them.
    int promptEvaluated = 0;   // timings.prompt_n: the prompt tokens it evaluated (the rest came from the slot's cache).
    int generated = 0;         // timings.predicted_n
    double promptMs = 0.0;     // timings.prompt_ms
    double generateMs = 0.0;   // timings.predicted_ms
};

// Streams one POST /completion to `url`: each message's text goes to `onPiece` as it arrives, and the last message's
// stop_type and timings fill the result. A stop closes the connection at once, even while the server is still reading
// the prompt and sends nothing (llama-server then cancels the task), and the result is `cancelled`. Throws
// InferenceError naming the URL for an unreachable server, an HTTP error, a line that is not a message, a message
// without its fields, an error message in the stream, an unknown stop_type, or a stream that ends before its last
// message.
StreamResult StreamCompletion(const std::string& url, const nlohmann::json& body,
                              const std::function<void(std::string_view)>& onPiece, std::stop_token stop)
{
    StreamResult result;
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    httplib::Client client(url);
    SetTimeouts(client);
    std::stop_callback closeOnStop(stop, [&client] { client.stop(); });

    // The status arrives before the body: an error's body is kept whole for the message, a stream is read line by
    // line as it arrives. A failure inside the receiver ends the request and is thrown once cpp-httplib returns.
    int status = 0;
    std::string errorBody;
    std::string pending; // Received bytes after the last complete line.
    bool finished = false;
    std::exception_ptr failure = nullptr;
    httplib::Request request;
    request.method = "POST";
    request.path = "/completion";
    request.body = BodyText(body);
    request.set_header("Content-Type", "application/json");
    request.response_handler = [&status](const httplib::Response& response) {
        status = response.status;
        return true;
    };
    request.content_receiver = [&](const char* pData, std::size_t length, std::size_t /*offset*/, std::size_t /*total*/) {
        if (status != 200) {
            errorBody.append(pData, length);
            return true;
        }
        try {
            pending.append(pData, length);
            // Server-sent events: "data: {json}" lines, blank lines between them, and ":" lines (pings) to skip.
            for (std::size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                if (stop.stop_requested()) {
                    return false;
                }
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty() || line.front() == ':') {
                    continue;
                }
                if (!line.starts_with("data: ")) {
                    throw InferenceError(std::format("{}/completion sent a line that is not a message: '{}'", url, Quoted(line)));
                }
                nlohmann::json message;
                try {
                    message = nlohmann::json::parse(std::string_view(line).substr(6));
                } catch (const nlohmann::json::parse_error&) {
                    throw InferenceError(std::format("{}/completion sent a message that is not JSON: '{}'", url, Quoted(line)));
                }
                if (message.contains("error")) {
                    throw InferenceError(std::format("{}/completion failed: {}", url, Quoted(message.at("error").dump())));
                }
                try {
                    // Every message carries the prompt's count and a piece of text; the last one says why generation
                    // stopped and how long each part took.
                    result.promptTokens = message.at("tokens_evaluated").get<int>();
                    const std::string& content = message.at("content").get_ref<const std::string&>();
                    if (!content.empty()) {
                        onPiece(content);
                    }
                    if (message.at("stop").get<bool>()) {
                        finished = true;
                        result.stopType = message.at("stop_type").get<std::string>();
                        const nlohmann::json& timings = message.at("timings");
                        result.promptEvaluated = timings.at("prompt_n").get<int>();
                        result.generated = timings.at("predicted_n").get<int>();
                        result.promptMs = timings.at("prompt_ms").get<double>();
                        result.generateMs = timings.at("predicted_ms").get<double>();
                    }
                } catch (const nlohmann::json::exception& error) {
                    throw InferenceError(std::format("{}/completion sent a message without its fields ({}): '{}'", url,
                                                     error.what(), Quoted(line)));
                }
                if (finished && result.stopType != "eos" && result.stopType != "limit" && result.stopType != "word") {
                    throw InferenceError(std::format("{}/completion stopped for an unknown reason '{}'", url, result.stopType));
                }
            }
            return true;
        } catch (...) {
            failure = std::current_exception();
            return false;
        }
    };

    const httplib::Result response = client.send(request);
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (!finished && stop.stop_requested()) {
        // The stop closed the connection (or came as the reply ended): the call is cancelled, as in-process.
        result.cancelled = true;
        return result;
    }
    if (!response) {
        throw InferenceError(std::format("cannot reach {}/completion: {}", url, httplib::to_string(response.error())));
    }
    if (status != 200) {
        throw InferenceError(std::format("{}/completion answered HTTP {}: {}", url, status, Quoted(errorBody)));
    }
    if (!finished) {
        throw InferenceError(std::format("the stream from {}/completion ended before its last message", url));
    }
    return result;
}
```

Replace the stub

```cpp
inference::Completion LlamaServerBackend::Complete(const inference::Prompt& /*prompt*/,
                                                   const inference::GenerationSpec& /*spec*/,
                                                   const inference::TokenCallback& /*onToken*/, std::stop_token /*stop*/)
{
    throw InferenceError("LlamaServerBackend::Complete is not implemented yet");
}
```

with

```cpp
inference::Completion LlamaServerBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                   const inference::TokenCallback& onToken, std::stop_token stop)
{
    MORPHEUS_REQUIRE(spec.maxTokens > 0, "Complete: maxTokens must be positive");
    MORPHEUS_REQUIRE(spec.thinkingMaxTokens >= 0, "Complete: thinkingMaxTokens must not be negative");
    MORPHEUS_REQUIRE(spec.slot >= -1, "Complete: slot must be -1 (the configured slot) or a server slot");

    // Thinking is asked of the model: it must have a thinking mode, and the budget must hold the closing sentence.
    const bool think = spec.thinkingMaxTokens > 0;
    if (think && !_thinking) {
        throw InferenceError(std::format("the chat model at {} has no thinking mode, so it cannot think before "
                                         "answering (set the thinking budgets to 0)",
                                         _config.chatUrl));
    }
    MORPHEUS_REQUIRE(!think || spec.thinkingMaxTokens > static_cast<int>(_thinking->close.size()),
                     "Complete: thinkingMaxTokens is too small to close the thinking block");

    // The prompt's tokens, made once for this call: a thinking model's end with the open block when the call thinks
    // (in place of the empty block), as in-process. They must leave room for the thinking and the answer.
    std::vector<std::int32_t> tokens = TokenizePrompt(prompt);
    if (think) {
        tokens.resize(tokens.size() - _thinking->skip.size());
        tokens.insert(tokens.end(), _thinking->open.begin(), _thinking->open.end());
    }
    inference::Completion completion;
    completion.promptTokens = static_cast<int>(tokens.size());
    if (spec.thinkingMaxTokens + spec.maxTokens > ContextSize() - completion.promptTokens) {
        throw InferenceError(std::format("prompt ({} tokens) plus thinking ({}) and max_tokens ({}) exceeds the "
                                         "context size ({})",
                                         completion.promptTokens, spec.thinkingMaxTokens, spec.maxTokens, ContextSize()));
    }

    // What every call of this completion sends. The prompt as token ids: /completion evaluates them as they are (it
    // adds BOS only to a prompt given as text). The sampling is LlamaCppBackend's chain: top-k, top-p keeping at least
    // one token, temperature, then llama-server's draw with the seed (at temperature 0 only the most likely token is
    // left: greedy, as in-process). Every other sampler llama-server knows is named neutral, so its command-line
    // defaults (penalties, DRY, top-n-sigma, typical, min-p, XTC, mirostat, a logit bias) cannot change what the model
    // writes. Then the slot, whose cache the calls share, and the stream.
    nlohmann::json body = {{"prompt", tokens},
                           {"samplers", nlohmann::json::array({"top_k", "top_p", "temperature"})},
                           {"top_k", spec.sampling.topK},
                           {"top_p", spec.sampling.topP},
                           {"min_keep", 1},
                           {"temperature", spec.sampling.temperature},
                           {"dynatemp_range", 0.0},
                           {"dynatemp_exponent", 1.0},
                           {"seed", spec.sampling.seed},
                           {"mirostat", 0},
                           {"min_p", 0.0},
                           {"typical_p", 1.0},
                           {"top_n_sigma", -1.0},
                           {"xtc_probability", 0.0},
                           {"repeat_penalty", 1.0},
                           {"repeat_last_n", 0},
                           {"frequency_penalty", 0.0},
                           {"presence_penalty", 0.0},
                           {"dry_multiplier", 0.0},
                           {"logit_bias", nlohmann::json::array()},
                           {"ignore_eos", false},
                           {"n_probs", 0},
                           {"id_slot", spec.slot == -1 ? _config.slot : spec.slot},
                           {"cache_prompt", true},
                           {"stream", true}};
    // The first call reads the prompt: its numbers are the prompt's. The server must have evaluated exactly the tokens
    // sent, since the budgets rely on that count.
    auto takePromptNumbers = [&](const StreamResult& result) {
        if (result.promptTokens != completion.promptTokens) {
            throw InferenceError(std::format("{} counted {} prompt tokens where Morpheus sent {}",
                                             _config.chatUrl, result.promptTokens, completion.promptTokens));
        }
        completion.tokensEvaluated = result.promptEvaluated;
        completion.tokensReused = completion.promptTokens - result.promptEvaluated;
        completion.promptMs = result.promptMs;
    };

    // Think: one call without the grammar and without streaming to onToken, stopping at the end marker or at the
    // budget less the closing sentence. Its text is the thinking.
    if (think) {
        nlohmann::json thinking = body;
        thinking["n_predict"] = spec.thinkingMaxTokens - static_cast<int>(_thinking->close.size());
        thinking["stop"] = nlohmann::json::array({std::string(inference::THINKING_END_TEXT)});
        const StreamResult thought = StreamCompletion(
            _config.chatUrl, thinking, [&completion](std::string_view piece) { completion.thinking += piece; }, stop);
        if (thought.cancelled) {
            completion.cancelled = true;
            return completion;
        }
        takePromptNumbers(thought);
        completion.thinkingTokens = thought.generated;
        completion.thinkingMs = thought.generateMs;
        if (thought.stopType == "eos") {
            return completion; // The model ended its reply inside the block: there is no answer.
        }
        // The answer call continues the same tokens. The stream cannot give the ids the model wrote (llama-server sends
        // no message for a token that ends inside a UTF-8 character, and in stream mode its last message carries no
        // ids), so the thinking text is tokenized again, as plain text: where the model's own split differs, the
        // server's cache reuse stops there.
        auto append = [&tokens](const std::vector<std::int32_t>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
        if (!completion.thinking.empty()) {
            append(ServerTokens(completion.thinking, false, false));
        }
        if (thought.stopType == "word") {
            // The model closed the block: the end marker (the stop word, which the server leaves out of the text),
            // then the blank line its template puts before the answer.
            append(_thinking->end);
            append(_thinking->afterEnd);
            completion.thinkingTokens += static_cast<int>(_thinking->afterEnd.size());
        } else {
            // Out of budget ("limit"): close the block for the model.
            append(_thinking->close);
            completion.thinkingTokens += static_cast<int>(_thinking->close.size());
            completion.thinkingStoppedAtLimit = true;
        }
        body["prompt"] = tokens;
    }

    // Answer: under the grammar, each piece streamed to onToken.
    nlohmann::json answer = body;
    answer["n_predict"] = spec.maxTokens;
    if (!spec.grammar.empty()) {
        answer["grammar"] = spec.grammar;
    }
    const StreamResult answered = StreamCompletion(
        _config.chatUrl, answer,
        [&](std::string_view piece) {
            completion.text += piece;
            if (onToken) {
                onToken(piece);
            }
        },
        stop);
    if (answered.cancelled) {
        completion.cancelled = true;
        return completion;
    }
    if (think) {
        completion.thinkingMs += answered.promptMs; // Reading the block back is part of thinking.
    } else {
        takePromptNumbers(answered);
    }
    completion.tokensGenerated = answered.generated;
    completion.generateMs = answered.generateMs;
    completion.stoppedAtLimit = answered.stopType == "limit";
    return completion;
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver]"`
Expected: builds with no warnings; `All tests passed` (21 test cases).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 400`.

- [ ] **Step 5: Run the AddressSanitizer preset on the new tests**

Run: `cmake --preset asan && cmake --build --preset asan && ./build/asan/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"`
Expected: `All tests passed`, no AddressSanitizer report (the stream receiver, the stop callback and the fake's threads touch shared buffers).

- [ ] **Step 6: Byte check and commit**

```bash
check_bytes src/llamaserver/LlamaServerBackend.cpp tests/llamaserver/LlamaServerCompleteTests.cpp
git add src/llamaserver/LlamaServerBackend.cpp tests/llamaserver/LlamaServerCompleteTests.cpp
git commit -m "LlamaServerBackend::Complete: token prompts, in-process sampling, thinking in two calls, slots, telemetry, cancel" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 7: The backend keys in settings.json

`settings.json` names its backend. Under `llama-cpp` the model keys stay required; under `llama-server` a `server` object is, and the model keys are ignored. `context_size`, the prefixes, the budget and the sampling stay required for both.

**Files:**
- Modify: `src/core/Settings.h`, `src/core/Settings.cpp`
- Modify: `settings.example.json`
- Test: `tests/core/SettingsTests.cpp`
- This machine only, never staged: `settings.json`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: in `core/Settings.h`: `enum class BackendKind { LlamaCpp, LlamaServer };`, `std::optional<BackendKind> ParseBackendKind(std::string_view text)` (`"llama-cpp"`, `"llama-server"`, else nullopt), `std::string BackendKindName(BackendKind kind)`; `core::Settings` fields `BackendKind backend = BackendKind::LlamaCpp`, `std::string chatUrl`, `std::string embeddingUrl`, `int serverSlot = 0`. `LoadSettings` validation messages name the key (`settings needs 'backend'`, `'backend' must be 'llama-cpp' or 'llama-server', got '...'`, `settings needs 'server'`, `server needs 'chat_url'`, `server 'chat_url' must start with 'http://' ...`, `server 'slot' must be an integer in 0..2147483647`, `unknown key '...' in server`). `SettingsToJson` writes `backend`, plus `chat_model`, `embedding_model`, `gpu_layers` under `llama-cpp` or `server` under `llama-server`.

- [ ] **Step 1: Write the failing tests**

In `tests/core/SettingsTests.cpp`, in `ValidSettingsJson()`, make `"backend": "llama-cpp",` the document's first key (before `"chat_model"`).

In the test case "SettingsToJson round-trips through LoadSettings", add after `CHECK(copy.chatModel == original.chatModel);`:

```cpp
    CHECK(copy.backend == BackendKind::LlamaCpp);
```

In the test case "settings.example.json loads with the current loader", add after `const Settings settings = LoadSettings(dir / "settings.json");`:

```cpp
    CHECK(settings.backend == BackendKind::LlamaCpp); // The example runs the models in-process.
```

Append:

```cpp
TEST_CASE("LoadSettings reads the llama-server backend", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["backend"] = "llama-server";
    json["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://localhost:8081"}, {"slot", 2}};
    json.erase("chat_model");
    json.erase("embedding_model");
    json.erase("gpu_layers");
    const Settings settings = LoadSettings(WriteSettings("settings_server", json));
    CHECK(settings.backend == BackendKind::LlamaServer);
    CHECK(settings.chatUrl == "http://127.0.0.1:8080");
    CHECK(settings.embeddingUrl == "http://localhost:8081");
    CHECK(settings.serverSlot == 2);
    CHECK(settings.chatModel.empty());
    CHECK(settings.contextSize == 16384); // Still what the budgets plan for.
    CHECK(settings.budget.narrateMaxTokens == 768);

    // The in-process backend's keys are ignored when present, even ones that would not load.
    json["chat_model"] = "models/missing.gguf";
    json["gpu_layers"] = "all";
    CHECK(LoadSettings(WriteSettings("settings_server_ignores_models", json)).chatModel.empty());

    // And llama-cpp ignores a server object.
    nlohmann::json local = ValidSettingsJson();
    local["server"] = {{"chat_url", "not a url"}};
    CHECK(LoadSettings(WriteSettings("settings_cpp_ignores_server", local)).backend == BackendKind::LlamaCpp);
}

TEST_CASE("LoadSettings names the backend key at fault", "[settings]")
{
    // Writes `json` and checks that loading it is a DataError naming `key`.
    auto refuses = [](const std::string& name, const nlohmann::json& json, const std::string& key) {
        INFO(name);
        const fs::path file = WriteSettings(name, json);
        CHECK_THROWS_AS(LoadSettings(file), DataError);
        CHECK_THROWS_WITH(LoadSettings(file), ContainsSubstring(key));
    };

    nlohmann::json missing = ValidSettingsJson();
    missing.erase("backend");
    refuses("settings_backend_missing", missing, "backend");
    nlohmann::json unknown = ValidSettingsJson();
    unknown["backend"] = "ollama";
    refuses("settings_backend_unknown", unknown, "backend");

    nlohmann::json server = ValidSettingsJson();
    server["backend"] = "llama-server";
    refuses("settings_server_missing", server, "server");
    server["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 0}};
    for (const std::string key : {"chat_url", "embedding_url", "slot"}) {
        nlohmann::json without = server;
        without["server"].erase(key);
        refuses("settings_server_no_" + key, without, key);
    }
    nlohmann::json https = server;
    https["server"]["chat_url"] = "https://127.0.0.1:8080"; // HTTPS is not supported.
    refuses("settings_server_https", https, "chat_url");
    nlohmann::json bare = server;
    bare["server"]["embedding_url"] = "127.0.0.1:8081";
    refuses("settings_server_bare", bare, "embedding_url");
    nlohmann::json empty = server;
    empty["server"]["chat_url"] = "http://";
    refuses("settings_server_empty_host", empty, "chat_url");
    nlohmann::json negative = server;
    negative["server"]["slot"] = -1;
    refuses("settings_server_slot", negative, "slot");
    nlohmann::json extra = server;
    extra["server"]["port"] = 8080;
    refuses("settings_server_extra", extra, "port");

    // llama-cpp still needs its model keys.
    for (const std::string key : {"chat_model", "embedding_model", "gpu_layers"}) {
        nlohmann::json without = ValidSettingsJson();
        without.erase(key);
        refuses("settings_cpp_no_" + key, without, key);
    }
}

TEST_CASE("SettingsToJson writes the backend and its own keys", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["backend"] = "llama-server";
    json["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 1}};
    const Settings original = LoadSettings(WriteSettings("settings_server_to_json_a", json));
    const nlohmann::json written = SettingsToJson(original);
    CHECK(written.at("backend") == "llama-server");
    CHECK(written.at("server") == json.at("server"));
    CHECK_FALSE(written.contains("chat_model")); // Only the backend in use has its keys written.
    CHECK_FALSE(written.contains("gpu_layers"));
    const Settings copy = LoadSettings(WriteSettings("settings_server_to_json_b", written));
    CHECK(copy.backend == BackendKind::LlamaServer);
    CHECK(copy.chatUrl == original.chatUrl);
    CHECK(copy.embeddingUrl == original.embeddingUrl);
    CHECK(copy.serverSlot == 1);

    // The in-process backend writes its model keys and no server object.
    const nlohmann::json local = SettingsToJson(LoadSettings(WriteSettings("settings_cpp_to_json", ValidSettingsJson())));
    CHECK(local.at("backend") == "llama-cpp");
    CHECK(local.contains("chat_model"));
    CHECK(local.contains("gpu_layers"));
    CHECK_FALSE(local.contains("server"));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/core/SettingsTests.cpp`: `'BackendKind': undeclared identifier`, `'backend': is not a member of 'morpheus::core::Settings'`.

- [ ] **Step 3: Add the backend to Settings**

In `src/core/Settings.h`, add `#include <string_view>` to the standard includes, and after the `MIN_THINKING_TOKENS` constant insert:

```cpp

// Which backend the app and the tools build: llama.cpp in-process, or llama-server over HTTP.
enum class BackendKind { LlamaCpp, LlamaServer };

// "llama-cpp" or "llama-server"; nullopt for anything else.
std::optional<BackendKind> ParseBackendKind(std::string_view text);

// The settings.json spelling: "llama-cpp" or "llama-server".
std::string BackendKindName(BackendKind kind);
```

In `struct Settings`, replace

```cpp
struct Settings {
    std::filesystem::path chatModel = {};      // Relative paths are resolved against the settings file's folder.
    std::filesystem::path embeddingModel = {};
```

with

```cpp
struct Settings {
    BackendKind backend = BackendKind::LlamaCpp;
    // llama-cpp only (empty under llama-server): the model files. Relative paths are resolved against the settings
    // file's folder.
    std::filesystem::path chatModel = {};
    std::filesystem::path embeddingModel = {};
```

replace

```cpp
    int gpuLayers = 0;                         // -1 puts every layer on the GPU.
```

with

```cpp
    int gpuLayers = 0;                         // llama-cpp only. -1 puts every layer on the GPU.
    // llama-server only (empty under llama-cpp): "http://host:port" of the chat and embedding servers, and the server
    // slot Morpheus's own calls use.
    std::string chatUrl = {};
    std::string embeddingUrl = {};
    int serverSlot = 0;
```

and replace the comment of `LoadSettings`

```cpp
// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors,
// both model files must exist, and the budget must leave room for the transcript (L3) and SETTINGS_TEMPLATE_MARGIN
// chat-template tokens; the review budgets are not part of that sum. memory_mode is optional; every other key is
// required.
```

with

```cpp
// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors.
// `backend` says which keys the models need: under llama-cpp chat_model, embedding_model (both files must exist) and
// gpu_layers; under llama-server a `server` object with chat_url and embedding_url (each "http://" and a host) and
// slot (at least 0). Each backend's keys are ignored, not checked, under the other. The budget must leave room for the
// transcript (L3) and SETTINGS_TEMPLATE_MARGIN chat-template tokens; the review budgets are not part of that sum.
// memory_mode is optional; every other key is required.
```

and the comment of `SettingsToJson`

```cpp
// The settings as settings.json JSON, with model paths absolute (UTF-8). Stored in a run's meta for reference.
```

with

```cpp
// The settings as settings.json JSON, with model paths absolute (UTF-8), and only the keys of the backend in use.
// Stored in a run's meta for reference.
```

In `src/core/Settings.cpp`, add `#include <string_view>` to the standard includes. After the anonymous namespace (before `Settings LoadSettings(`), add:

```cpp
std::optional<BackendKind> ParseBackendKind(std::string_view text)
{
    if (text == "llama-cpp") {
        return BackendKind::LlamaCpp;
    }
    if (text == "llama-server") {
        return BackendKind::LlamaServer;
    }
    return std::nullopt;
}

std::string BackendKindName(BackendKind kind)
{
    switch (kind) {
    case BackendKind::LlamaCpp:
        return "llama-cpp";
    case BackendKind::LlamaServer:
        return "llama-server";
    }
    throw ContractError("BackendKindName: unhandled BackendKind");
}

```

In `LoadSettings`, replace

```cpp
    checkKeys(json, "settings", {"chat_model", "embedding_model", "embedding_query_prefix", "embedding_document_prefix",
                                 "context_size", "gpu_layers", "budget", "sampling", "memory_mode", "narration_check",
                                 "compact_transcript", "prose_turns"});
```

with

```cpp
    checkKeys(json, "settings", {"backend", "chat_model", "embedding_model", "embedding_query_prefix",
                                 "embedding_document_prefix", "context_size", "gpu_layers", "server", "budget",
                                 "sampling", "memory_mode", "narration_check", "compact_transcript", "prose_turns"});
```

and replace

```cpp
    settings.chatModel = requireModel("chat_model");
    settings.embeddingModel = requireModel("embedding_model");
    settings.embeddingQueryPrefix = requireString(json, "embedding_query_prefix", "settings");
    settings.embeddingDocumentPrefix = requireString(json, "embedding_document_prefix", "settings");
    settings.contextSize = requireInt(json, "context_size", "settings", 512, 262144);
    settings.gpuLayers = requireInt(json, "gpu_layers", "settings", -1, 10000);
```

with

```cpp
    // The backend says which keys reach the models. Each backend's own keys are ignored (not read or checked) under
    // the other, so a file can keep both while it switches.
    const std::string backendText = requireString(json, "backend", "settings");
    const std::optional<BackendKind> backend = ParseBackendKind(backendText);
    if (!backend) {
        throw DataError(std::format("{}: 'backend' must be 'llama-cpp' or 'llama-server', got '{}'", where, backendText));
    }
    settings.backend = *backend;
    if (settings.backend == BackendKind::LlamaCpp) {
        settings.chatModel = requireModel("chat_model");
        settings.embeddingModel = requireModel("embedding_model");
        settings.gpuLayers = requireInt(json, "gpu_layers", "settings", -1, 10000);
    } else {
        // The servers: plain HTTP to a host (localhost is the intended use), and the slot Morpheus's calls use.
        const nlohmann::json& server = requireMember(json, "server", "settings");
        checkKeys(server, "server", {"chat_url", "embedding_url", "slot"});
        auto requireUrl = [&](const char* key) {
            const std::string url = requireString(server, key, "server");
            if (!url.starts_with("http://") || url.size() == std::string_view("http://").size()) {
                throw DataError(std::format("{}: server '{}' must start with 'http://' and name a host (HTTPS is not "
                                            "supported), got '{}'",
                                            where, key, url));
            }
            return url;
        };
        settings.chatUrl = requireUrl("chat_url");
        settings.embeddingUrl = requireUrl("embedding_url");
        settings.serverSlot = requireInt(server, "slot", "server", 0, std::numeric_limits<int>::max());
    }
    settings.embeddingQueryPrefix = requireString(json, "embedding_query_prefix", "settings");
    settings.embeddingDocumentPrefix = requireString(json, "embedding_document_prefix", "settings");
    settings.contextSize = requireInt(json, "context_size", "settings", 512, 262144);
```

In `SettingsToJson`, replace

```cpp
    nlohmann::json json = {
        {"chat_model", PathText(settings.chatModel)},
        {"embedding_model", PathText(settings.embeddingModel)},
        {"embedding_query_prefix", settings.embeddingQueryPrefix},
        {"embedding_document_prefix", settings.embeddingDocumentPrefix},
        {"context_size", settings.contextSize},
        {"gpu_layers", settings.gpuLayers},
```

with

```cpp
    nlohmann::json json = {
        {"backend", BackendKindName(settings.backend)},
        {"embedding_query_prefix", settings.embeddingQueryPrefix},
        {"embedding_document_prefix", settings.embeddingDocumentPrefix},
        {"context_size", settings.contextSize},
```

and replace

```cpp
    if (settings.memoryMode) {
        json["memory_mode"] = *settings.memoryMode == MemoryMode::Engine ? "engine" : "assisted";
    }
    return json;
```

with

```cpp
    if (settings.memoryMode) {
        json["memory_mode"] = *settings.memoryMode == MemoryMode::Engine ? "engine" : "assisted";
    }
    // Only the backend in use has its keys.
    if (settings.backend == BackendKind::LlamaCpp) {
        json["chat_model"] = PathText(settings.chatModel);
        json["embedding_model"] = PathText(settings.embeddingModel);
        json["gpu_layers"] = settings.gpuLayers;
    } else {
        json["server"] = {{"chat_url", settings.chatUrl}, {"embedding_url", settings.embeddingUrl}, {"slot", settings.serverSlot}};
    }
    return json;
```

In `settings.example.json`, add `"backend": "llama-cpp",` as the first key:

```json
{
  "backend": "llama-cpp",
  "chat_model": "models/Qwen3.8-27B-UD-Q4_K_M.gguf",
```

(the rest of the file unchanged).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings]"`
Expected: `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 403` (the app tests copy `settings.example.json`, which now names its backend).

- [ ] **Step 5: Update this machine's settings.json (never staged)**

This machine's `settings.json` (gitignored) has no `backend` key, so the app and the tools would refuse it now. Add `"backend": "llama-cpp",` as its first key, by hand or with:

```bash
perl -0pi -e 's/\A\{(\r?\n)/{$1  "backend": "llama-cpp",$1/' settings.json && head -3 settings.json
```

Expected: the second line is `  "backend": "llama-cpp",`. Then check it loads: `cmake --build build/dev --config RelWithDebInfo --target morpheus_probe` and `./build/dev/bin/RelWithDebInfo/morpheus_probe.exe settings.json game_projects/sandcrawler "I look around."` streams two completions (it needs the GPU; skip the run if the models are not in `models/`). `git status --short` must not list `settings.json`.

- [ ] **Step 6: Byte check and commit**

```bash
check_bytes src/core/Settings.h src/core/Settings.cpp settings.example.json tests/core/SettingsTests.cpp
git add src/core/Settings.h src/core/Settings.cpp settings.example.json tests/core/SettingsTests.cpp
git commit -m "settings.json chooses the backend: llama-cpp or llama-server with its server URLs and slot" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 8: The backend in the Settings window

`SettingsEdit` reads and writes `backend` and the server keys; the Settings window edits the backend (a choice) and, under `llama-server`, the two URLs and the slot; the model-path fields and GPU layers show only under `llama-cpp`.

**Files:**
- Modify: `src/app/model/SettingsEdit.h`, `src/app/model/SettingsEdit.cpp`
- Modify: `src/app/ui/SettingsWindow.cpp`
- Test: `tests/app/SettingsEditTests.cpp`, `tests/app/UiTests.cpp`

**Interfaces:**
- Consumes: `core::BackendKind`, `core::ParseBackendKind`, `core::BackendKindName`, `core::Settings::backend`, `chatUrl`, `embeddingUrl`, `serverSlot` (Task 7).
- Produces: `app::SettingsEdit` fields `core::BackendKind backend = core::BackendKind::LlamaCpp`, `std::string chatUrl`, `std::string embeddingUrl`, `int serverSlot = 0`. `ReadSettingsEdit` requires the active backend's keys and reads the other backend's keys when present (empty or 0 otherwise); `SaveSettingsEdit` writes `backend` and the active backend's keys, and keeps the other backend's keys as the file has them.

- [ ] **Step 1: Write the failing tests**

In `tests/app/SettingsEditTests.cpp`, in the test case "ReadSettingsEdit reads the edited values as the file writes them", add after `const app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");`:

```cpp
    CHECK(edit.backend == core::BackendKind::LlamaCpp); // As settings.example.json sets it.
    CHECK(edit.chatUrl.empty());                        // The example has no server object.
```

Append:

```cpp
TEST_CASE("SaveSettingsEdit switches the backend to llama-server and back, keeping the other backend's keys",
          "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_server");
    const std::filesystem::path file = dir / "settings.json";
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    const std::string chatModel = edit.chatModel;

    edit.backend = core::BackendKind::LlamaServer;
    edit.chatUrl = "http://127.0.0.1:8080";
    edit.embeddingUrl = "http://127.0.0.1:8081";
    edit.serverSlot = 2;
    const core::Settings server = app::SaveSettingsEdit(file, edit);
    CHECK(server.backend == core::BackendKind::LlamaServer);
    CHECK(server.chatUrl == "http://127.0.0.1:8080");
    CHECK(server.embeddingUrl == "http://127.0.0.1:8081");
    CHECK(server.serverSlot == 2);
    const nlohmann::json written = nlohmann::json::parse(test::ReadFile(file));
    CHECK(written.at("backend") == "llama-server");
    CHECK(written.at("server").at("slot") == 2);
    CHECK(written.at("chat_model") == chatModel); // The in-process backend's keys stay, ignored.

    // Read back, then switched back: the server keys stay in the file for the next switch.
    app::SettingsEdit reread = app::ReadSettingsEdit(file);
    CHECK(reread.backend == core::BackendKind::LlamaServer);
    CHECK(reread.chatUrl == "http://127.0.0.1:8080");
    CHECK(reread.serverSlot == 2);
    CHECK(reread.chatModel == chatModel);
    reread.backend = core::BackendKind::LlamaCpp;
    const core::Settings local = app::SaveSettingsEdit(file, reread);
    CHECK(local.backend == core::BackendKind::LlamaCpp);
    CHECK(nlohmann::json::parse(test::ReadFile(file)).at("server").at("chat_url") == "http://127.0.0.1:8080");
    CHECK(app::ReadSettingsEdit(file).embeddingUrl == "http://127.0.0.1:8081");
}

TEST_CASE("SaveSettingsEdit refuses a llama-server edit without its URLs", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_server_invalid");
    const std::filesystem::path file = dir / "settings.json";
    const std::string before = test::ReadFile(file);
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.backend = core::BackendKind::LlamaServer;
    edit.embeddingUrl = "http://127.0.0.1:8081"; // chat_url stays empty.

    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("chat_url"));
    CHECK(test::ReadFile(file) == before);
}

TEST_CASE("ReadSettingsEdit reads a llama-server file without model keys", "[settings_edit]")
{
    const std::filesystem::path dir = test::FreshTempDir("settings_edit_server_only");
    nlohmann::json json = nlohmann::json::parse(test::ReadFile(test::RepoDir() / "settings.example.json"));
    json.erase("chat_model");
    json.erase("embedding_model");
    json.erase("gpu_layers");
    json["backend"] = "llama-server";
    json["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 1}};
    test::WriteFile(dir / "settings.json", json.dump(2));

    const app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    CHECK(edit.backend == core::BackendKind::LlamaServer);
    CHECK(edit.chatModel.empty());
    CHECK(edit.serverSlot == 1);
    // Saving it writes no model keys.
    const core::Settings saved = app::SaveSettingsEdit(dir / "settings.json", edit);
    CHECK(saved.backend == core::BackendKind::LlamaServer);
    CHECK_FALSE(nlohmann::json::parse(test::ReadFile(dir / "settings.json")).contains("chat_model"));
}
```

In `tests/app/UiTests.cpp`, add `#include "core/Settings.h"` after `#include "core/MemoryMode.h"`, and append this draw smoke test (the window's state is private; the test proves the llama-server branch draws without an ImGui assertion):

```cpp
TEST_CASE("The Settings window draws the llama-server backend's fields", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_settings_server");
    // settings.json on llama-server, without the in-process backend's keys: the URLs and the slot replace them.
    nlohmann::json settings = nlohmann::json::parse(test::ReadFile(dir / "settings.json"));
    settings.erase("chat_model");
    settings.erase("embedding_model");
    settings.erase("gpu_layers");
    settings["backend"] = "llama-server";
    settings["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 0}};
    test::WriteFile(dir / "settings.json", settings.dump(2));
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::SettingsWindow window;

    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    for (int frame = 0; frame < 2; ++frame) {
        controller.Frame();
        imgui.Frame([&] { window.Draw(controller); });
    }
    CHECK(controller.Engine().CurrentSettings().backend == core::BackendKind::LlamaServer);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/app/SettingsEditTests.cpp`: `'backend': is not a member of 'morpheus::app::SettingsEdit'` (and `chatUrl`, `embeddingUrl`, `serverSlot`).

- [ ] **Step 3: Read and write the backend keys**

In `src/app/model/SettingsEdit.h`, replace

```cpp
struct SettingsEdit {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int contextSize = 0;
    int gpuLayers = 0; // -1 puts every layer on the GPU.
```

with

```cpp
struct SettingsEdit {
    core::BackendKind backend = core::BackendKind::LlamaCpp;
    // llama-cpp: the models and their GPU layers. Kept as the file has them under llama-server ("" and 0 when absent).
    std::string chatModel = {};
    std::string embeddingModel = {};
    int gpuLayers = 0; // -1 puts every layer on the GPU.
    // llama-server: server.chat_url, server.embedding_url and server.slot. Kept as the file has them under llama-cpp
    // ("" and 0 when absent).
    std::string chatUrl = {};
    std::string embeddingUrl = {};
    int serverSlot = 0;
    int contextSize = 0;
```

and replace the comment of `ReadSettingsEdit`

```cpp
// Reads the edited values from a settings file. Throws DataError naming the file when it cannot be read, is not
// JSON, or lacks one of them (core::LoadSettings explains a bad file in full).
```

with

```cpp
// Reads the edited values from a settings file: the backend in use needs its keys, the other backend's keys are read
// when present. Throws DataError naming the file when it cannot be read, is not JSON, or lacks one of the values it
// needs (core::LoadSettings explains a bad file in full).
```

and the first sentence of the comment of `SaveSettingsEdit`

```cpp
// Writes `edit` into the settings file, keeping its other keys (the embedding prefixes, the rest of the budget) and
// their order.
```

with

```cpp
// Writes `edit` into the settings file: `backend` and the keys of the backend it names (the other backend's keys stay
// as the file has them), keeping the file's other keys (the embedding prefixes, the rest of the budget) and their
// order.
```

In `src/app/model/SettingsEdit.cpp`, in `ReadSettingsEdit`, replace

```cpp
        SettingsEdit edit;
        edit.chatModel = json.at("chat_model").get<std::string>();
        edit.embeddingModel = json.at("embedding_model").get<std::string>();
        edit.contextSize = json.at("context_size").get<int>();
        edit.gpuLayers = json.at("gpu_layers").get<int>();
```

with

```cpp
        SettingsEdit edit;
        const std::string backendText = json.at("backend").get<std::string>();
        const std::optional<core::BackendKind> backend = core::ParseBackendKind(backendText);
        if (!backend) {
            throw core::DataError(
                std::format("{}: 'backend' must be 'llama-cpp' or 'llama-server'", core::PathText(file)));
        }
        edit.backend = *backend;
        // The backend in use needs its keys; the other backend's are read when the file has them, so a switch in the
        // window starts from them.
        const bool local = edit.backend == core::BackendKind::LlamaCpp;
        if (local || json.contains("chat_model")) {
            edit.chatModel = json.at("chat_model").get<std::string>();
        }
        if (local || json.contains("embedding_model")) {
            edit.embeddingModel = json.at("embedding_model").get<std::string>();
        }
        if (local || json.contains("gpu_layers")) {
            edit.gpuLayers = json.at("gpu_layers").get<int>();
        }
        if (!local || json.contains("server")) {
            const nlohmann::ordered_json& server = json.at("server");
            if (!local || server.contains("chat_url")) {
                edit.chatUrl = server.at("chat_url").get<std::string>();
            }
            if (!local || server.contains("embedding_url")) {
                edit.embeddingUrl = server.at("embedding_url").get<std::string>();
            }
            if (!local || server.contains("slot")) {
                edit.serverSlot = server.at("slot").get<int>();
            }
        }
        edit.contextSize = json.at("context_size").get<int>();
```

Add `#include <optional>` to the file's standard includes.

In `SaveSettingsEdit`, replace

```cpp
        json["chat_model"] = edit.chatModel;
        json["embedding_model"] = edit.embeddingModel;
        json["context_size"] = edit.contextSize;
        json["gpu_layers"] = edit.gpuLayers;
```

with

```cpp
        // The backend and its own keys; the other backend's keys stay as the file has them.
        json["backend"] = core::BackendKindName(edit.backend);
        if (edit.backend == core::BackendKind::LlamaCpp) {
            json["chat_model"] = edit.chatModel;
            json["embedding_model"] = edit.embeddingModel;
            json["gpu_layers"] = edit.gpuLayers;
        } else {
            json["server"]["chat_url"] = edit.chatUrl;
            json["server"]["embedding_url"] = edit.embeddingUrl;
            json["server"]["slot"] = edit.serverSlot;
        }
        json["context_size"] = edit.contextSize;
```

- [ ] **Step 4: Edit the backend in the Settings window**

In `src/app/ui/SettingsWindow.cpp`, add `#include <imgui_stdlib.h>` after `#include <imgui.h>`. In the anonymous namespace, after `MEMORY_MODES`, add:

```cpp
// Combo items, in core::BackendKind's order.
constexpr const char* BACKENDS[] = {"llama.cpp, in-process (llama-cpp)", "llama-server over HTTP (llama-server)"};
```

Replace

```cpp
            ImGui::SeparatorText("Models");
            modelCombo("Chat model", edit.chatModel);
            modelCombo("Embedding model", edit.embeddingModel);
            ImGui::InputInt("Context size", &edit.contextSize, 1024, 4096);
            ImGui::InputInt("GPU layers", &edit.gpuLayers);
            ImGui::SetItemTooltip("-1 puts every layer on the GPU.");
```

with

```cpp
            // The backend: models in-process, picked from the models folder, or two llama-server URLs and a slot.
            // Only the fields of the backend chosen show; the other's stay in settings.json.
            ImGui::SeparatorText("Models");
            int backend = edit.backend == core::BackendKind::LlamaCpp ? 0 : 1;
            if (ImGui::Combo("Backend", &backend, BACKENDS, IM_ARRAYSIZE(BACKENDS))) {
                edit.backend = backend == 0 ? core::BackendKind::LlamaCpp : core::BackendKind::LlamaServer;
            }
            if (edit.backend == core::BackendKind::LlamaCpp) {
                modelCombo("Chat model", edit.chatModel);
                modelCombo("Embedding model", edit.embeddingModel);
            } else {
                ImGui::InputText("Chat server URL", &edit.chatUrl);
                ImGui::SetItemTooltip("http://host:port of the chat model's llama-server, e.g. http://127.0.0.1:8080.");
                ImGui::InputText("Embedding server URL", &edit.embeddingUrl);
                ImGui::SetItemTooltip("http://host:port of the embedding model's llama-server (started with "
                                      "--embedding), e.g. http://127.0.0.1:8081.");
                ImGui::InputInt("Server slot", &edit.serverSlot);
                ImGui::SetItemTooltip("The chat server's slot for Morpheus's calls (0 or more). Each slot keeps its "
                                      "own prompt cache.");
            }
            ImGui::InputInt("Context size", &edit.contextSize, 1024, 4096);
            if (edit.backend == core::BackendKind::LlamaServer) {
                ImGui::SetItemTooltip("What the budgets plan for. Each of the chat server's slots must hold at least "
                                      "this many tokens.");
            }
            if (edit.backend == core::BackendKind::LlamaCpp) {
                ImGui::InputInt("GPU layers", &edit.gpuLayers);
                ImGui::SetItemTooltip("-1 puts every layer on the GPU.");
            }
```

and replace

```cpp
            ImGui::TextDisabled("Apply saves settings.json and restarts the engine. New model paths, context size or "
                                "GPU layers reload the models.");
```

with

```cpp
            ImGui::TextDisabled("Apply saves settings.json and restarts the engine. A new backend, model paths, server "
                                "URLs or slot, context size or GPU layers reload the backend.");
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit],[ui]"`
Expected: `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 407`.

- [ ] **Step 6: Byte check and commit**

```bash
check_bytes src/app/model/SettingsEdit.h src/app/model/SettingsEdit.cpp src/app/ui/SettingsWindow.cpp tests/app/SettingsEditTests.cpp tests/app/UiTests.cpp
git add src/app/model/SettingsEdit.h src/app/model/SettingsEdit.cpp src/app/ui/SettingsWindow.cpp tests/app/SettingsEditTests.cpp tests/app/UiTests.cpp
git commit -m "Edit the backend, the server URLs and the slot in the Settings window" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 9: The callers build the backend settings.json names

The engine host keeps a loaded backend only while the backend kind, the URLs and the slot are unchanged too; the app chooses its factory by `settings.backend`; `morpheus_play` and `morpheus_probe` build either backend.

**Files:**
- Modify: `src/app/model/EngineHost.h`, `src/app/model/EngineHost.cpp`
- Modify: `src/app/main.cpp`, `src/app/CMakeLists.txt`
- Modify: `tools/play/main.cpp`, `tools/play/CMakeLists.txt`
- Modify: `tools/probe/main.cpp`, `tools/probe/CMakeLists.txt`
- Test: `tests/app/EngineHostTests.cpp`, `tests/app/AppControllerTests.cpp`

**Interfaces:**
- Consumes: `core::Settings::backend`, `chatUrl`, `embeddingUrl`, `serverSlot` (Task 7); `llamaserver::LlamaServerBackend`, `LlamaServerConfig` (Tasks 5-6); `app::SettingsEdit`'s backend fields (Task 8).
- Produces: `app::EngineHost::BackendFactory app::BackendFactoryByKind(EngineHost::BackendFactory llamaCpp, EngineHost::BackendFactory llamaServer)` in `app/model/EngineHost.h`.

- [ ] **Step 1: Write the failing tests**

In `tests/app/EngineHostTests.cpp`, append:

```cpp
TEST_CASE("EngineHost rebuilds the backend when the backend kind, a server URL or the slot changes", "[engine_host]")
{
    std::atomic<int> made = 0;
    app::EngineHost host(CountingFactory(made));
    core::Settings server = test::EngineSettings();
    server.backend = core::BackendKind::LlamaServer;
    server.chatUrl = "http://127.0.0.1:8080";
    server.embeddingUrl = "http://127.0.0.1:8081";
    host.Start(server);
    PollWhileLoading(host);
    CHECK(made == 1);

    // The same servers: the backend is kept.
    host.Start(server);
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Ready);
    CHECK(made == 1);

    // Each change loads a new one.
    core::Settings moved = server;
    moved.chatUrl = "http://127.0.0.1:9090";
    host.Start(moved);
    PollWhileLoading(host);
    CHECK(made == 2);
    core::Settings otherEmbedding = moved;
    otherEmbedding.embeddingUrl = "http://127.0.0.1:9091";
    host.Start(otherEmbedding);
    PollWhileLoading(host);
    CHECK(made == 3);
    core::Settings otherSlot = otherEmbedding;
    otherSlot.serverSlot = 1;
    host.Start(otherSlot);
    PollWhileLoading(host);
    CHECK(made == 4);
    host.Start(test::EngineSettings()); // Back to llama-cpp.
    PollWhileLoading(host);
    CHECK(made == 5);
}
```

In `tests/app/AppControllerTests.cpp`, append:

```cpp
TEST_CASE("The app's backend factory builds the backend settings.json names", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_backend_kind");
    // Recording factories in place of llama.cpp and llama-server: both make a ScriptedBackend and count their calls.
    std::atomic<int> llamaCpp = 0;
    std::atomic<int> llamaServer = 0;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder("You walk."));
    app::AppController controller(test::PathsIn(dir), app::BackendFactoryByKind(
                                                          [&](const core::Settings& settings) {
                                                              ++llamaCpp;
                                                              return scripted(settings);
                                                          },
                                                          [&](const core::Settings& settings) {
                                                              ++llamaServer;
                                                              return scripted(settings);
                                                          }));
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK(llamaCpp == 1); // settings.example.json names llama-cpp.
    CHECK(llamaServer == 0);

    app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    edit.backend = core::BackendKind::LlamaServer;
    edit.chatUrl = "http://127.0.0.1:8080";
    edit.embeddingUrl = "http://127.0.0.1:8081";
    REQUIRE(controller.CanApplySettings());
    CHECK(controller.ApplySettings(edit).empty());
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK(llamaCpp == 1);
    CHECK(llamaServer == 1);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error in `tests/app/AppControllerTests.cpp`: `'BackendFactoryByKind': is not a member of 'morpheus::app'`.

Comment the new AppController test out, build, and run `./build/tests/bin/Debug/morpheus_tests.exe "[engine_host]"`.
Expected: FAIL in "EngineHost rebuilds the backend ...": `CHECK( made == 2 )` with expansion `1 == 2` (a URL change keeps the backend today). Uncomment the test.

- [ ] **Step 3: Rebuild on a backend change, and choose the factory by kind**

In `src/app/model/EngineHost.h`, replace the comment of `Start`

```cpp
    // Starts the engine with `settings`. The running worker stops first, cancelling its command and closing its
    // run. The backend is kept when the model paths, embedding prefixes, context size and GPU layers are unchanged,
    // and the engine is Ready at once. Otherwise the backend is released (freeing its memory) and a new one loads in
    // the background. Throws ContractError while Loading: a load cannot be interrupted.
```

with

```cpp
    // Starts the engine with `settings`. The running worker stops first, cancelling its command and closing its
    // run. The backend is kept when the backend kind, the model paths, the server URLs and slot, the embedding
    // prefixes, the context size and the GPU layers are unchanged, and the engine is Ready at once. Otherwise the
    // backend is released (freeing its memory) and a new one loads in the background. Throws ContractError while
    // Loading: a load cannot be interrupted.
```

and after the class's closing `};` add:

```cpp

// A factory that makes the backend settings.backend names: `llamaCpp` for BackendKind::LlamaCpp, `llamaServer` for
// BackendKind::LlamaServer. The app passes the llama.cpp and llama-server factories (main.cpp); the tests pass
// recording ones. Throws ContractError when either is empty.
EngineHost::BackendFactory BackendFactoryByKind(EngineHost::BackendFactory llamaCpp, EngineHost::BackendFactory llamaServer);
```

In `src/app/model/EngineHost.cpp`, replace

```cpp
    // Whether the loaded backend was made for the same models, prefixes and context.
    const bool sameBackend = _pBackend && settings.chatModel == _settings.chatModel &&
                             settings.embeddingModel == _settings.embeddingModel &&
                             settings.embeddingQueryPrefix == _settings.embeddingQueryPrefix &&
                             settings.embeddingDocumentPrefix == _settings.embeddingDocumentPrefix &&
                             settings.contextSize == _settings.contextSize && settings.gpuLayers == _settings.gpuLayers;
```

with

```cpp
    // Whether the loaded backend was made for the same kind of backend, models or servers, prefixes and context.
    const bool sameBackend = _pBackend && settings.backend == _settings.backend &&
                             settings.chatModel == _settings.chatModel &&
                             settings.embeddingModel == _settings.embeddingModel &&
                             settings.chatUrl == _settings.chatUrl && settings.embeddingUrl == _settings.embeddingUrl &&
                             settings.serverSlot == _settings.serverSlot &&
                             settings.embeddingQueryPrefix == _settings.embeddingQueryPrefix &&
                             settings.embeddingDocumentPrefix == _settings.embeddingDocumentPrefix &&
                             settings.contextSize == _settings.contextSize && settings.gpuLayers == _settings.gpuLayers;
```

and at the end of the file, before `} // namespace morpheus::app`, add:

```cpp
EngineHost::BackendFactory BackendFactoryByKind(EngineHost::BackendFactory llamaCpp, EngineHost::BackendFactory llamaServer)
{
    MORPHEUS_REQUIRE(llamaCpp && llamaServer, "BackendFactoryByKind: both factories are needed");
    return [llamaCpp = std::move(llamaCpp), llamaServer = std::move(llamaServer)](
               const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        switch (settings.backend) {
        case core::BackendKind::LlamaCpp:
            return llamaCpp(settings);
        case core::BackendKind::LlamaServer:
            return llamaServer(settings);
        }
        throw core::ContractError("BackendFactoryByKind: unhandled BackendKind");
    };
}

```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[engine_host],[app_controller]"`
Expected: `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 409`.

- [ ] **Step 5: Build the backend settings.json names in the app, play and probe**

In `src/app/main.cpp`, add `#include "llamaserver/LlamaServerBackend.h"` after `#include "llama/LlamaCppBackend.h"`. After `MakeLlamaBackend`, add:

```cpp

// The llama-server backend for the engine's settings: the chat and embedding servers they name. Runs on EngineHost's
// loader thread.
std::unique_ptr<inference::LlmBackend> MakeServerBackend(const core::Settings& settings)
{
    llamaserver::LlamaServerConfig config;
    config.chatUrl = settings.chatUrl;
    config.embeddingUrl = settings.embeddingUrl;
    config.slot = settings.serverSlot;
    config.contextSize = settings.contextSize;
    config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
    config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
    return std::make_unique<llamaserver::LlamaServerBackend>(config);
}
```

and replace

```cpp
        app::AppController controller(app::AppController::PathsUnder(root), MakeLlamaBackend);
```

with

```cpp
        app::AppController controller(app::AppController::PathsUnder(root),
                                      app::BackendFactoryByKind(MakeLlamaBackend, MakeServerBackend));
```

Also update `MakeLlamaBackend`'s comment line `// The llama.cpp backend for the engine's settings. Runs on EngineHost's loader thread.` to `// The llama.cpp backend for the engine's settings: the models they name. Runs on EngineHost's loader thread.`

In `src/app/CMakeLists.txt`, replace

```cmake
    # The app: the SDL3 window, ImGui's SDL backends and the llama.cpp backend.
    add_executable(morpheus main.cpp)
    target_link_libraries(morpheus PRIVATE morpheus_app_ui morpheus_llama imgui_sdl3)
```

with

```cmake
    # The app: the SDL3 window, ImGui's SDL backends, and the llama.cpp and llama-server backends.
    add_executable(morpheus main.cpp)
    target_link_libraries(morpheus PRIVATE morpheus_app_ui morpheus_llama morpheus_llamaserver imgui_sdl3)
```

In `tools/play/main.cpp`, add `#include <memory>` to the standard includes and `#include "llamaserver/LlamaServerBackend.h"` after `#include "llama/LlamaCppBackend.h"`, and replace

```cpp
        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend llamaBackend(config);
        std::optional<turn::RecordingBackend> recording;
        if (recordFile) {
            recording.emplace(llamaBackend);
        }
        inference::LlmBackend& backend =
            recording ? static_cast<inference::LlmBackend&>(*recording) : static_cast<inference::LlmBackend&>(llamaBackend);
```

with

```cpp
        // The backend settings.json names: llama.cpp in-process, or llama-server over HTTP.
        std::unique_ptr<inference::LlmBackend> pModels = nullptr;
        if (settings.backend == core::BackendKind::LlamaCpp) {
            llama::LlamaConfig config;
            config.chatModel = settings.chatModel;
            config.embeddingModel = settings.embeddingModel;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            config.contextSize = settings.contextSize;
            config.gpuLayers = settings.gpuLayers;
            pModels = std::make_unique<llama::LlamaCppBackend>(config);
        } else {
            llamaserver::LlamaServerConfig config;
            config.chatUrl = settings.chatUrl;
            config.embeddingUrl = settings.embeddingUrl;
            config.slot = settings.serverSlot;
            config.contextSize = settings.contextSize;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            pModels = std::make_unique<llamaserver::LlamaServerBackend>(config);
        }
        std::optional<turn::RecordingBackend> recording;
        if (recordFile) {
            recording.emplace(*pModels);
        }
        inference::LlmBackend& backend = recording ? static_cast<inference::LlmBackend&>(*recording) : *pModels;
```

In `tools/play/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_play PRIVATE morpheus_turn morpheus_llama)
```

with

```cmake
target_link_libraries(morpheus_play PRIVATE morpheus_turn morpheus_llama morpheus_llamaserver)
```

In `tools/probe/main.cpp`, replace the header comment's first three lines

```cpp
// morpheus_probe: loads settings.json and a world, assembles a resolve-style prompt, and streams two
// completions, printing token usage and KV-cache reuse. A manual check that the model, the Vulkan backend and
// prefix caching work on this machine.
```

with

```cpp
// morpheus_probe: loads settings.json and a world, assembles a resolve-style prompt, and streams two
// completions, printing token usage and KV-cache reuse. A manual check that the model, the GPU backend and
// prefix caching work on this machine; with "backend": "llama-server", the reuse figures are the server's
// (its timings), so the second run measures what a llama-server slot reuses.
```

Add `#include <memory>` to its standard includes and `#include "llamaserver/LlamaServerBackend.h"` after `#include "llama/LlamaCppBackend.h"`, and replace

```cpp
        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend backend(config);
```

with

```cpp
        // The backend settings.json names: llama.cpp in-process, or llama-server over HTTP.
        std::unique_ptr<inference::LlmBackend> pBackend = nullptr;
        if (settings.backend == core::BackendKind::LlamaCpp) {
            llama::LlamaConfig config;
            config.chatModel = settings.chatModel;
            config.embeddingModel = settings.embeddingModel;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            config.contextSize = settings.contextSize;
            config.gpuLayers = settings.gpuLayers;
            pBackend = std::make_unique<llama::LlamaCppBackend>(config);
        } else {
            llamaserver::LlamaServerConfig config;
            config.chatUrl = settings.chatUrl;
            config.embeddingUrl = settings.embeddingUrl;
            config.slot = settings.serverSlot;
            config.contextSize = settings.contextSize;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            pBackend = std::make_unique<llamaserver::LlamaServerBackend>(config);
            std::cout << std::format("backend: llama-server at {} (slot {})\n", settings.chatUrl, settings.serverSlot);
        }
        inference::LlmBackend& backend = *pBackend;
```

In `tools/probe/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_turn morpheus_llama)
```

with

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_turn morpheus_llama morpheus_llamaserver)
```

- [ ] **Step 6: Build the dev targets and smoke-check them**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 3`
Expected: exits with 0 (the window, the settings with `"backend": "llama-cpp"` and the font start; the models begin loading).

Run (a llama-server settings file pointing at a port where nothing listens, to see the error path):

```bash
mkdir -p build/server-smoke && perl -0pe 's/"backend": "llama-cpp"/"backend": "llama-server", "server": {"chat_url": "http:\/\/127.0.0.1:1", "embedding_url": "http:\/\/127.0.0.1:1", "slot": 0}/' settings.example.json > build/server-smoke/settings.json
./build/dev/bin/RelWithDebInfo/morpheus_probe.exe build/server-smoke/settings.json game_projects/sandcrawler "I look around."; echo "exit $?"
```

Expected: `error: cannot reach http://127.0.0.1:1/props: ...` and `exit 1` (the probe built the llama-server backend from the settings).

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes src/app/model/EngineHost.h src/app/model/EngineHost.cpp src/app/main.cpp src/app/CMakeLists.txt tools/play/main.cpp tools/play/CMakeLists.txt tools/probe/main.cpp tools/probe/CMakeLists.txt tests/app/EngineHostTests.cpp tests/app/AppControllerTests.cpp
git add src/app/model/EngineHost.h src/app/model/EngineHost.cpp src/app/main.cpp src/app/CMakeLists.txt tools/play/main.cpp tools/play/CMakeLists.txt tools/probe/main.cpp tools/probe/CMakeLists.txt tests/app/EngineHostTests.cpp tests/app/AppControllerTests.cpp
git commit -m "The app, morpheus_play and morpheus_probe build the backend settings.json names" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 10: The [server] suite: a real llama-server, and R2's three-slot reuse

Tests against a `llama-server` the owner starts, skipped unless `MORPHEUS_TEST_SERVER_URL` and `MORPHEUS_TEST_EMBED_SERVER_URL` are set. **No llama-server runs on this machine: every test of this task is skipped here.** They are written to compile and skip cleanly; the owner runs them (README, Task 11) and records the reuse figures.

**Files:**
- Create: `tests/TestEnvironment.h`
- Modify: `tests/llama/LlamaTestModels.h`
- Create: `tests/llamaserver/LiveServer.h`
- Test: `tests/llamaserver/LlamaServerLiveTests.cpp` (`[server]`, every preset with the llama-server backend)
- Test: `tests/llama/LlamaServerAgreementTests.cpp` (`[server][llama]`, dev preset only)

**Interfaces:**
- Consumes: `llamaserver::LlamaServerBackend` (Tasks 5-6), `grammar::Check` (Task 2), `test::SharedBackend()` (`tests/llama/LlamaTestModels.h`).
- Produces: `std::string morpheus::test::EnvironmentVariable(const char* name)` in `tests/TestEnvironment.h` (moved from `LlamaTestModels.h`, unchanged); `constexpr int test::LIVE_CONTEXT_SIZE = 16384` and `llamaserver::LlamaServerConfig test::LiveServerConfig(int slot)` in `tests/llamaserver/LiveServer.h`.

- [ ] **Step 1: Move EnvironmentVariable out of the llama-only header**

Create `tests/TestEnvironment.h`:

```cpp
#pragma once

#include <cstdlib>
#include <string>

namespace morpheus::test {

// Reads an environment variable; "" when unset. _dupenv_s avoids MSVC's getenv deprecation warning under /WX.
inline std::string EnvironmentVariable(const char* name)
{
#ifdef _MSC_VER
    char* pValue = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&pValue, &length, name) != 0 || pValue == nullptr) {
        return {};
    }
    std::string value(pValue);
    std::free(pValue);
    return value;
#else
    const char* pValue = std::getenv(name);
    return pValue != nullptr ? pValue : "";
#endif
}

} // namespace morpheus::test
```

In `tests/llama/LlamaTestModels.h`, delete the `EnvironmentVariable` function with its comment, delete `#include <cstdlib>`, and add `#include "TestEnvironment.h"` before `#include "llama/LlamaCppBackend.h"`.

- [ ] **Step 2: Write the [server] tests**

Create `tests/llamaserver/LiveServer.h`:

```cpp
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "TestEnvironment.h"
#include "llamaserver/LlamaServerBackend.h"

namespace morpheus::test {

// The [server] tests run against a llama-server the owner starts (README, "The [server] tests"):
//   MORPHEUS_TEST_SERVER_URL        the chat server, e.g. http://127.0.0.1:8080, started with --parallel 3 or more and
//                                   at least LIVE_CONTEXT_SIZE tokens per slot
//   MORPHEUS_TEST_EMBED_SERVER_URL  the embedding server (--embedding), e.g. http://127.0.0.1:8081
// The calling test is skipped when either is unset.
constexpr int LIVE_CONTEXT_SIZE = 16384;

// A config for `slot` of the owner's servers, with nomic-embed's prefixes. Skips the calling test when a variable is
// unset; call it on the test's own thread.
inline llamaserver::LlamaServerConfig LiveServerConfig(int slot)
{
    const std::string chatUrl = EnvironmentVariable("MORPHEUS_TEST_SERVER_URL");
    const std::string embeddingUrl = EnvironmentVariable("MORPHEUS_TEST_EMBED_SERVER_URL");
    if (chatUrl.empty() || embeddingUrl.empty()) {
        SKIP("set MORPHEUS_TEST_SERVER_URL and MORPHEUS_TEST_EMBED_SERVER_URL to run the [server] tests");
    }
    llamaserver::LlamaServerConfig config;
    config.chatUrl = chatUrl;
    config.embeddingUrl = embeddingUrl;
    config.slot = slot;
    config.contextSize = LIVE_CONTEXT_SIZE;
    config.embeddingQueryPrefix = "search_query: ";
    config.embeddingDocumentPrefix = "search_document: ";
    return config;
}

} // namespace morpheus::test
```

Create `tests/llamaserver/LlamaServerLiveTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/MemoryMode.h"
#include "core/World.h"
#include "grammar/Check.h"
#include "inference/Llm.h"
#include "llamaserver/LiveServer.h"
#include "llamaserver/LlamaServerBackend.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;

namespace {

constexpr int SESSIONS = 3;
constexpr int EXCHANGES = 10;
constexpr int LORE_LINES = 60; // About 2,500 tokens of system prompt per session.

// The action call's grammar: one of a few actions, as the Skyrim host's action catalog allows.
const std::string ACTION_GRAMMAR = "root ::= \"{\\\"action\\\":\\\"\" (\"wait\" | \"follow\" | \"trade\" | \"attack\") \"\\\"}\"\n";

// Session `session`'s system prompt: a character of its own and a long block of what it knows, so the prefix its slot
// keeps is worth reusing and differs from the other sessions'.
std::string CharacterPrompt(int session)
{
    static const std::array<const char*, SESSIONS> NAMES = {"Hulda", "Saadia", "Belethor"};
    const char* pName = NAMES[static_cast<std::size_t>(session)];
    std::string prompt = std::format("You are {0}, who lives in the town of Whiterun. Stay in character and answer as "
                                     "{0} would.\n\n# What {0} knows\n",
                                     pName);
    for (int line = 0; line < LORE_LINES; ++line) {
        prompt += std::format("- {} heard rumour {}: the caravan from the south was {} days late, and the guards say "
                              "the road past milestone {} is unsafe after dark.\n",
                              pName, line, (line * 7 + session) % 13 + 1, line * 3 + session);
    }
    return prompt;
}

// What the player says in exchange `exchange`.
std::string PlayerLine(int exchange)
{
    static const std::array<const char*, 5> LINES = {"Good day to you.", "Have you heard any news from the road?",
                                                     "I am looking for work.", "What do you think of the guards?",
                                                     "Farewell for now."};
    return LINES[static_cast<std::size_t>(exchange) % LINES.size()];
}

// The share of a call's prompt the server served from the slot's cache.
double ReusedFraction(const inference::Completion& completion)
{
    return completion.promptTokens == 0 ? 0.0 : static_cast<double>(completion.tokensReused) / completion.promptTokens;
}

} // namespace

TEST_CASE("llama-server: a resolve call completes under its grammar and a narrate call streams", "[server]")
{
    llamaserver::LlamaServerBackend backend(test::LiveServerConfig(0));
    const core::World world = core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");

    inference::Prompt prompt;
    prompt.messages = {
        {inference::Role::System,
         "You are the game master of a text RPG. Reply with one JSON object: intent, checks, changes.\n\n" +
             context::RenderWorldReference(world)},
        {inference::Role::User, "# Current state\n" + context::RenderStateView(world.initialState) +
                                    "\n# Player\nI climb onto the crawler's roof to scan the horizon.\n\n# Task\n"
                                    "Resolve the action."},
    };
    prompt.stableCount = 1;

    inference::GenerationSpec resolve;
    resolve.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);
    resolve.maxTokens = 1024;
    const inference::Completion resolved = backend.Complete(prompt, resolve, {}, {});
    WARN(std::format("resolve: {} prompt tokens ({} reused), {} generated in {:.0f} ms: {}", resolved.promptTokens,
                     resolved.tokensReused, resolved.tokensGenerated, resolved.generateMs, resolved.text));
    REQUIRE_FALSE(resolved.stoppedAtLimit);
    CHECK(grammar::Check(resolve.grammar, resolved.text) == "");
    const nlohmann::json output = nlohmann::json::parse(resolved.text);
    CHECK(output.at("intent").is_string());
    CHECK(output.at("checks").is_array());
    CHECK(output.at("changes").is_array());

    // The narrate call extends the resolve prompt on the same slot, so the server reuses part of it.
    inference::Prompt narrate = prompt;
    narrate.messages.push_back({inference::Role::Assistant, resolved.text});
    narrate.messages.push_back({inference::Role::User, "Narrate what happens in two sentences."});
    inference::GenerationSpec prose;
    prose.maxTokens = 128;
    std::string streamed;
    const inference::Completion narrated =
        backend.Complete(narrate, prose, [&](std::string_view piece) { streamed += piece; }, {});
    WARN(std::format("narrate: {} of {} prompt tokens reused: {}", narrated.tokensReused, narrated.promptTokens,
                     narrated.text));
    CHECK_FALSE(narrated.text.empty());
    CHECK(streamed == narrated.text);
}

TEST_CASE("llama-server: a thinking model thinks before it answers under the grammar", "[server]")
{
    llamaserver::LlamaServerBackend backend(test::LiveServerConfig(0));
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::User, "Is sea water salty? Answer yes or no."}};
    inference::GenerationSpec spec;
    spec.grammar = "root ::= \"yes\" | \"no\"\n";
    spec.maxTokens = 8;
    spec.thinkingMaxTokens = 256;

    inference::Completion completion;
    try {
        completion = backend.Complete(prompt, spec, {}, {});
    } catch (const inference::InferenceError& error) {
        if (std::string_view(error.what()).find("no thinking mode") == std::string_view::npos) {
            throw;
        }
        SKIP("the chat server's model has no thinking mode");
    }
    WARN(std::format("thinking: {} tokens in {:.0f} ms{}; answer '{}'", completion.thinkingTokens, completion.thinkingMs,
                     completion.thinkingStoppedAtLimit ? " (closed at the budget)" : "", completion.text));
    CHECK(completion.thinkingTokens > 0);
    CHECK(completion.thinkingTokens <= spec.thinkingMaxTokens);
    CHECK((completion.text == "yes" || completion.text == "no"));
}

TEST_CASE("llama-server: three sessions on three slots reuse their own cached prompts", "[server]")
{
    // R2's acceptance: three sessions, each on its own slot, alternate an action call (under a grammar) and a speech
    // call, ten exchanges each, on a 16k context. The configs are read here: SKIP must run on the test's thread.
    std::array<llamaserver::LlamaServerConfig, SESSIONS> configs;
    for (int session = 0; session < SESSIONS; ++session) {
        configs[static_cast<std::size_t>(session)] = test::LiveServerConfig(session);
    }

    // Each session runs at once on a thread of its own, with its own backend, as sub-project D's conversations will.
    // Catch2's checks are not thread-safe, so the threads only record; the checks follow.
    struct SessionLog {
        std::vector<std::string> calls = {};
        std::vector<double> reused = {};
        std::string error = {};
    };
    std::array<SessionLog, SESSIONS> logs;
    {
        std::vector<std::jthread> sessions;
        for (int session = 0; session < SESSIONS; ++session) {
            sessions.emplace_back([&configs, &logs, session] {
                SessionLog& log = logs[static_cast<std::size_t>(session)];
                try {
                    llamaserver::LlamaServerBackend backend(configs[static_cast<std::size_t>(session)]);
                    inference::Prompt conversation;
                    conversation.messages = {{inference::Role::System, CharacterPrompt(session)}};
                    conversation.stableCount = 1;
                    for (int exchange = 0; exchange < EXCHANGES; ++exchange) {
                        const std::string said = std::format("The player says: \"{}\"", PlayerLine(exchange));

                        // The action call: the conversation so far, then the request for an action.
                        inference::Prompt action = conversation;
                        action.messages.push_back({inference::Role::User, said + " Choose your action."});
                        inference::GenerationSpec actionSpec;
                        actionSpec.grammar = ACTION_GRAMMAR;
                        actionSpec.maxTokens = 16;
                        const inference::Completion acted = backend.Complete(action, actionSpec, {}, {});
                        log.calls.push_back("action " + acted.text);
                        log.reused.push_back(ReusedFraction(acted));

                        // The speech call: the same conversation, then the request for a line of speech.
                        inference::Prompt speech = conversation;
                        speech.messages.push_back({inference::Role::User, said + " Answer in one sentence."});
                        inference::GenerationSpec speechSpec;
                        speechSpec.maxTokens = 48;
                        const inference::Completion spoke = backend.Complete(speech, speechSpec, {}, {});
                        log.calls.push_back("speech");
                        log.reused.push_back(ReusedFraction(spoke));

                        // The exchange joins the conversation the next calls extend.
                        conversation.messages.push_back({inference::Role::User, said});
                        conversation.messages.push_back({inference::Role::Assistant, spoke.text});
                    }
                } catch (const std::exception& error) {
                    log.error = error.what();
                }
            });
        }
    } // The sessions join here.

    for (int session = 0; session < SESSIONS; ++session) {
        const SessionLog& log = logs[static_cast<std::size_t>(session)];
        INFO("session " << session);
        CHECK(log.error == "");
        REQUIRE(log.reused.size() == static_cast<std::size_t>(2 * EXCHANGES));
        // Every call after the session's first finds part of its prompt in its own slot's cache. The figures go to
        // the README; 80% is not asserted, since what a model's linear-attention layers allow is what is measured.
        std::string figures;
        double laterSum = 0.0;
        for (std::size_t call = 0; call < log.reused.size(); ++call) {
            figures += std::format("{:.2f} ", log.reused[call]);
            if (call > 0) {
                INFO("call " << call << " (" << log.calls[call] << ")");
                CHECK(log.reused[call] > 0.0);
                laterSum += log.reused[call];
            }
        }
        WARN(std::format("session {} (slot {}): reused fraction per call: {}; mean after the first: {:.2f}", session,
                         configs[static_cast<std::size_t>(session)].slot, figures,
                         laterSum / static_cast<double>(log.reused.size() - 1)));
    }
}
```

Create `tests/llama/LlamaServerAgreementTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

#include "LlamaTestModels.h"
#include "TestEnvironment.h"
#include "inference/Llm.h"
#include "llamaserver/LiveServer.h"
#include "llamaserver/LlamaServerBackend.h"

using namespace morpheus;

TEST_CASE("llama-server and the in-process backend count the same tokens", "[server][llama]")
{
    llama::LlamaCppBackend& local = test::SharedBackend();
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(0);

    // The same model on both sides: the server's model file must be MORPHEUS_TEST_CHAT_MODEL's.
    httplib::Client client(config.chatUrl);
    const httplib::Result props = client.Get("/props");
    REQUIRE(props);
    REQUIRE(props->status == 200);
    const std::string serverModel =
        std::filesystem::path(nlohmann::json::parse(props->body).at("model_path").get<std::string>()).filename().string();
    const std::string localModel =
        std::filesystem::path(test::EnvironmentVariable("MORPHEUS_TEST_CHAT_MODEL")).filename().string();
    if (serverModel != localModel) {
        SKIP("the chat server runs " + serverModel + ", not MORPHEUS_TEST_CHAT_MODEL's " + localModel);
    }
    llamaserver::LlamaServerBackend server(config);

    for (const std::string text : {"", "The crawler groans over the dunes.", "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC",
                                   R"({"intent":"climb","checks":[]})", "  leading spaces and\nnew lines\n"}) {
        INFO(text);
        CHECK(server.CountTokens(text) == local.CountTokens(text));
    }
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::System, "You are the game master."},
                       {inference::Role::User, "I climb the dune."},
                       {inference::Role::Assistant, R"({"intent":"climb","checks":[],"changes":[]})"},
                       {inference::Role::User, "Narrate it."}};
    CHECK(server.CountPromptTokens(prompt) == local.CountPromptTokens(prompt));
}
```

- [ ] **Step 3: Run the tests (they skip here)**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[server]"`
Expected: builds with no warnings; the three test cases are reported as skipped ("set MORPHEUS_TEST_SERVER_URL and MORPHEUS_TEST_EMBED_SERVER_URL ..."), none failed.

Run: `ctest --preset tests`
Expected: `0 tests failed out of 412`, with the three `llama-server: ...` tests listed as "Not Run (Skipped)".

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests && ./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[server]"`
Expected: builds with no warnings; four test cases skipped (the agreement test skips at `LiveServerConfig`, or at `SharedBackend` when the model variables are unset).

- [ ] **Step 4: Byte check and commit**

```bash
check_bytes tests/TestEnvironment.h tests/llama/LlamaTestModels.h tests/llamaserver/LiveServer.h tests/llamaserver/LlamaServerLiveTests.cpp tests/llama/LlamaServerAgreementTests.cpp
git add tests/TestEnvironment.h tests/llama/LlamaTestModels.h tests/llamaserver/LiveServer.h tests/llamaserver/LlamaServerLiveTests.cpp tests/llama/LlamaServerAgreementTests.cpp
git commit -m "Add the [server] tests: a real llama-server, token agreement, and R2's three-slot reuse" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Expected: `check_bytes` prints nothing (`LlamaServerAgreementTests.cpp`'s non-ASCII text must stay `\x` escapes).

---

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

