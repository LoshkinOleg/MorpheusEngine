# Model access over HTTP (sub-project C) - Design

Date: 2026-11-15. Source: requirement R2, option (a), in `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` (the Skyrim/Mantella host), with the owner's decisions. Third of the integration's sub-projects A-G; A (consumable build) and B (grammar library) are done.

## Goal

Morpheus can reach its models through `llama-server` over HTTP, as well as in-process.

- One model host can then serve several consumers: Morpheus's own game, `morpheus_server` (sub-project D), and the Skyrim side's other calls.
- Calls on separate server slots keep their cached prompt prefixes apart.
- It shows how much prompt-cache reuse Qwen3.5 actually gets through `llama-server`. In-process it gets none, because llama.cpp cannot cut its linear-attention cache back to a prefix.

## Owner decisions

- Morpheus's own app, `morpheus_play` and `morpheus_probe` can use the HTTP backend, chosen in `settings.json`.
- The grammar check becomes backend-independent: the test-only GBNF matcher is promoted into `morpheus_grammar`.
- The HTTP library is cpp-httplib (header-only, MIT; both client and server, so sub-project D and the tests' fake server use it too).
- The real-server tests run against a `llama-server` the owner provides (`MORPHEUS_TEST_SERVER_URL`). Morpheus does not build `llama-server`.

## 1. Interfaces (`inference`)

### The split

`inference::LlmBackend` (`src/inference/Llm.h`) is split into three interfaces:

```cpp
class Tokenizer {
public:
    virtual ~Tokenizer() = default;
    virtual int CountTokens(std::string_view text) = 0;
    virtual int CountPromptTokens(const Prompt& prompt) = 0;
    virtual int ContextSize() const = 0;
};

class ChatModel {
public:
    virtual ~ChatModel() = default;
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(std::span<const std::uint8_t> state) = 0;
};

class Embedder {
public:
    virtual ~Embedder() = default;
    virtual std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, EmbedPurpose purpose) = 0;
    virtual std::string EmbeddingModelId() const = 0;
};

class LlmBackend : public Tokenizer, public ChatModel, public Embedder {};
```

- Each method keeps its current contract and comment.
- Callers that use `LlmBackend` do not change; later code (sub-project D) can depend on just one part.

### `CheckGrammar` leaves the interface

`LlmBackend::CheckGrammar` is removed, and every implementation drops its override.

- The model seat's Check calls `grammar::Check` (section 3).
- `LlamaCppBackend` keeps its llama.cpp-based checker as a plain public method, `std::string CheckGrammar(const std::string& grammar, std::string_view text)`, with its current behaviour. The model-backed tests use it to confirm that `grammar::Check` agrees with llama.cpp.
- `turn::RecordingBackend`, `app::ModelSeatBackend`, `FakeBackend` and `ScriptedBackend` lose their `CheckGrammar` methods. In `ScriptedBackend`, the grammar-verdict test hooks go away.

### Slots

`GenerationSpec` gains `int slot = -1;`.

- -1 means the backend's default slot.
- The in-process backend ignores it, since it has one cache.
- `LlamaServerBackend` sends it as `id_slot`.
- Morpheus's own game leaves it at -1. Sub-project D sets one slot per conversation.

### Content markers move

The logic that keeps a message's content from becoming control tokens leaves `LlamaCppBackend::TokenizePrompt` for `inference`, so both backends share it (section 4, "Rendering a prompt"):

- `std::string ContentMarker(const Prompt&)`: the first of `"\x1E"`, `"\x1E1\x1E"`, `"\x1E2\x1E"`, ... that no content contains;
- `Prompt MarkContents(const Prompt&, std::string_view marker)`: every content replaced by the marker;
- `std::vector<std::string> TemplatePieces(std::string_view rendered, std::string_view marker, std::size_t contentCount)`: the template's text around the contents, an `InferenceError` when the template repeats or drops one.

`LlamaCppBackend`'s behaviour and error texts do not change.

### `SamplingFrom` moves

`inference::SamplingFrom(const core::SamplingSettings&, std::uint32_t)` moves to `turn` (`turn::SamplingFrom`). Its only callers are in `turn/Session.cpp`, and the move takes `inference`'s dependency on `core` away. The architecture spec's module table is updated to match.

## 2. Settings (`core`)

`settings.json` gains:

| Key | Type | Required | Meaning |
|---|---|---|---|
| `backend` | `"llama-cpp"` or `"llama-server"` | yes | Which backend the app and tools build |
| `server` | object | when `backend` is `llama-server` | `chat_url` (string), `embedding_url` (string), `slot` (integer, at least 0) |

- `chat_model`, `embedding_model` and `gpu_layers` are required only for `llama-cpp`. Under `llama-server` they may be absent; when present they are ignored.
- The embedding prefixes, `context_size`, the budget and sampling stay required for both.
- Under `llama-server`, `context_size` is still what the budgets are checked against. The backend checks at startup that the server's per-slot context is at least that large.
- **Validation:**
  - an unknown `backend` value, or a missing `server` object or `server` key under `llama-server`, is a `DataError` naming `settings.json` and the key;
  - `chat_url` and `embedding_url` must start with `http://` (localhost hosts are the intended use; HTTPS is not supported);
  - `slot` must be at least 0.
- `core::Settings` gains `backend` (enum `BackendKind { LlamaCpp, LlamaServer }`), `chatUrl`, `embeddingUrl` and `serverSlot`.
- `SettingsToJson` writes them, and `settings.example.json` sets `"backend": "llama-cpp"` with a commented `server` example in the README.
- This machine's `settings.json` gains `"backend": "llama-cpp"`; the implementer adds it and never stages it.
- **The Settings window:**
  - It edits `backend` (a choice) and, under `llama-server`, the two URLs and the slot.
  - The model-path fields show only under `llama-cpp`.
  - `SettingsEdit` reads and writes these keys.
  - The engine host rebuilds the backend when the backend kind, a URL or the slot changes, as it does today for a model path.

## 3. `grammar::Check` (`morpheus_grammar`)

The test-only matcher `tests/grammar/GbnfMatcher.h` moves into the library as `src/grammar/Check.h/.cpp`:

```cpp
// Whether `text` is accepted by `grammar` (GBNF, root rule "root") from start to end, matched by Unicode code point
// as llama.cpp's grammar sampler does. Returns "" when it is; otherwise "the grammar rejects '<c>' at the start",
// "the grammar rejects '<c>' after '<text>'" (the text cut to its last 60 bytes, with "..." in front when cut), or
// "the text ends before the grammar does". Throws ContractError for a grammar it cannot read.
std::string Check(const std::string& grammar, std::string_view text);
```

- **Where the reason points:** the furthest position any partial match reached. The rejected character is the code point there. If every path consumed the whole text but none finished, the text ended early.
- **Hardening carried over from the Task 1 review:**
  - repetition counts must be `digits` or `digits,` or `digits,digits` with min <= max; anything else is a read error;
  - matching is memoised by (node, position), so long texts and nested repetitions stay polynomial;
  - every read error is a `ContractError`.
- **What it does not model:** llama.cpp's load-time refusals (left recursion, the repetition limits). The builder and `FromSchema` stay within those limits by construction, and the model-backed tests remain the authority on them.
- **Tests:**
  - the test files switch from `test::GbnfMatcher` to `grammar::Check` (`Accepts(t)` becomes `Check(g, t).empty()`), and the test-only header is deleted;
  - the existing matcher, brute-force and property tests run on it;
  - new tests cover the reason texts, the hardening and the memoisation, for example a nested repetition over a 2,000-character text finishing quickly;
  - the model-backed agreement test compares `grammar::Check` with `LlamaCppBackend::CheckGrammar`, both accept/reject and the reason kind, on the random schema values and the `IntRange` probes.

## 4. `LlamaServerBackend` (new module `src/llamaserver`)

`llamaserver::LlamaServerBackend` (library `morpheus_llamaserver`, alias `Morpheus::llamaserver`) implements `LlmBackend` over HTTP with cpp-httplib.

- It depends on `inference`, `core` (for errors and UTF-8 helpers) and nlohmann/json.
- It is built in every preset, including the fast tests and vendored builds, behind the option `MORPHEUS_BUILD_LLAMA_SERVER` (default ON). cpp-httplib is fetched like the other dependencies, pinned by commit, with no OpenSSL.

### Configuration

`LlamaServerConfig { chatUrl, embeddingUrl, slot, contextSize, embeddingQueryPrefix, embeddingDocumentPrefix }`, built from `core::Settings` by the callers.

### Startup

The constructor reads `GET /props` from the chat server and from the embedding server.

- **Chat server:** it learns the per-slot context (`default_generation_settings.n_ctx`) and the chat template, and whether the template has the `enable_thinking` switch.
- **Embedding server:** it learns its model id.
- **Errors:**
  - a per-slot context smaller than `contextSize` is an `InferenceError` naming both numbers and the URL;
  - an unreachable server, an HTTP error or an unexpected response is an `InferenceError` naming the URL and what was wrong.
- **Context size:** `ContextSize()` returns `contextSize`, the settings value the budgets use.

### Token counts

- `CountTokens(text)` uses `POST /tokenize` (`{"content": text, "add_special": false, "parse_special": false}`).
- `CountPromptTokens(prompt)` is the length of the prompt's token list (below), the same list `Complete` sends.

### Rendering a prompt

Player text must never become control tokens: a content holding `<|im_end|>` is text. The backend therefore builds the prompt's tokens exactly as `LlamaCppBackend::TokenizePrompt` does in-process:

- **Markers.** Every message content is replaced by a per-call marker, the first of `"\x1E"`, `"\x1E1\x1E"`, `"\x1E2\x1E"`, ... that no content contains. The marker selection, the marking and the cutting of the rendered text are shared by both backends: `inference::ContentMarker`, `MarkContents` and `TemplatePieces` (moved out of `LlamaCppBackend`), not copied.
- **Rendering.** `POST /apply-template` with the marked messages and `add_generation_prompt: true`, plus `chat_template_kwargs: {"enable_thinking": true}` when the template has the switch. The rendered text ends where the assistant's answer begins.
- **Tokenizing.** The rendered text is cut at the markers; a template that repeats or drops a marker is an `InferenceError`, as in-process. Each template piece goes to `POST /tokenize` with `parse_special: true` (`add_special` only on the first piece, when the model adds BOS; a BOS the template wrote itself is not doubled); each content goes with `parse_special: false`. The ids are concatenated in order.
- **Thinking markers.** The backend then appends the thinking markers as tokens (tokenized once at startup with `parse_special`), exactly as `LlamaCppBackend` does: the empty block when thinking is off, the open marker when it is on.
- **Sending.** `/completion` receives the token list as its `prompt` (an array of ids, which llama-server evaluates as given).
- The token list is made once per prompt within one `Complete` call, not cached across calls.

### `Complete`

- **Answer without thinking:** one streaming `POST /completion` with:
  - `prompt`: the prompt's token ids (above);
  - `n_predict`: `maxTokens`;
  - `grammar`: when set;
  - sampling: the same chain as `LlamaCppBackend` (top-k, top-p keeping at least one token, temperature, then a draw with the seed; greedy at temperature 0). The request names it, `samplers: ["top_k", "top_p", "temperature"]` with `top_k`, `top_p`, `min_keep: 1`, `temperature`, `dynatemp_range: 0`, `dynatemp_exponent: 1` and `seed`, and sends neutral values for every other sampler llama-server knows, so its command-line defaults cannot change the output: `mirostat: 0`, `min_p: 0`, `typical_p: 1`, `top_n_sigma: -1`, `xtc_probability: 0`, `repeat_penalty: 1`, `repeat_last_n: 0`, `frequency_penalty: 0`, `presence_penalty: 0`, `dry_multiplier: 0`, `logit_bias: []`, `ignore_eos: false`, `n_probs: 0`;
  - `id_slot`: `spec.slot`, or the configured slot when -1;
  - `cache_prompt: true`, `stream: true`.

  Each streamed piece goes to `onToken`.
- **With thinking:** two calls, as the in-process backend does in one decode loop.
  1. The thinking call has no grammar and no streaming to `onToken`. It stops at the close marker (a `stop` string) or at `thinkingMaxTokens` minus the closing sentence's tokens; when it stops at the limit, the closing sentence is appended.
  2. The answer call continues that same prompt as tokens: the first call's ids, then the thinking text tokenized as plain text (`POST /tokenize`, `parse_special: false`), then the end marker and blank line, or the closing sentence, as tokens. It has the grammar and is streamed. The stream cannot give the generated ids themselves (llama-server sends no message for a token that ends inside a UTF-8 character, and its last stream message carries no ids), so the text is tokenized again; where the model's own split differs, cache reuse stops there.

  Because the second call extends the first, the server reuses its cache.
- **Telemetry:** the server's `timings` fill `Completion`:
  - `prompt_n` is the tokens evaluated, and `tokensReused` is the prompt tokens minus that;
  - `predicted_n` is the tokens generated;
  - `prompt_ms` and `predicted_ms` are the times.

  The thinking call fills the thinking fields. `stoppedAtLimit` comes from the server's `stop_type` / `limit` flags.
- **Cancel:** when `stop` is requested, the stream receiver returns false and the connection closes, so the server cancels the task. The `Completion` has `cancelled` set.
- **Contract checks:** the same `ContractError`s and `InferenceError`s as `LlamaCppBackend` (`maxTokens <= 0`, thinking asked of a model without a thinking mode, prompt plus generation over the context).

### Embeddings and state

- `Embed(texts, purpose)` posts each text, with its purpose's prefix, to `POST /embedding` on the embedding server. It L2-normalises the vector the server returns and checks every text got exactly one vector.
- `EmbeddingModelId()` is the embedding server's model id from `/props`.
- `SaveState` and `RestoreState` throw an `InferenceError` ("not supported by the llama-server backend"). Nothing calls them today.

### Threading

Like `LlamaCppBackend`, one thread owns the backend; it is not thread-safe. Several `LlamaServerBackend` objects may talk to one server at once (sub-project D), each on its own slot.

## 5. Callers

- `src/app/main.cpp`, `tools/play` and `tools/probe` build the backend from `settings.backend`: `LlamaCppBackend` from the model paths, or `LlamaServerBackend` from the URLs.
- `app::EngineHost`'s "keep the backend" rule also compares the backend kind, the URLs and the slot.
- `morpheus_probe` on `llama-server` reports the server's reuse figures on its second run, so the owner can measure Qwen3.5 through the server.

## 6. Tests (written first, Catch2)

### Fast suite (no model, no real server)

A fake `llama-server` (`tests/fakes/FakeLlamaServer.h`): a cpp-httplib server on a free local port that answers `/props`, `/tokenize`, `/apply-template`, `/completion` (streamed, with scripted pieces, timings and stop flags) and `/embedding` from a script, and records every request.

- **Startup:**
  - `/props` read;
  - a per-slot context smaller than `context_size` refused;
  - an unreachable URL refused with the URL in the message.
- **Token counts:** `CountTokens` and `CountPromptTokens` send the expected bodies and return the server's counts:
  - the marked messages to `/apply-template`, then each template piece with `parse_special: true` and each content with `parse_special: false`, in order;
  - a content holding `<|im_end|>` is tokenized with `parse_special: false`;
  - BOS on the first piece only, not doubled when the template writes it; a template that repeats or drops a content is refused;
  - the shared marker helpers (`ContentMarker`, `MarkContents`, `TemplatePieces`) have tests of their own.
- **`Complete`:**
  - the request body (prompt as token ids, grammar, the sampling chain and the neutral samplers, `id_slot` default and explicit, `cache_prompt`, `stream`);
  - pieces streamed in order;
  - telemetry from timings, including `tokensReused`;
  - `stoppedAtLimit`;
  - an HTTP 500 or a malformed stream line is an `InferenceError`.
- **Thinking:**
  - two calls, the second extending the first's tokens (the thinking text tokenized as plain text, then the marker tokens);
  - the thinking text kept out of `text` and `onToken`;
  - the limit closing sentence;
  - thinking asked of a non-thinking template is an `InferenceError`.
- **Cancel:** a stop requested mid-stream ends the call with `cancelled`, and the fake server sees the connection close.
- **Embeddings:** prefixes per purpose, normalisation, one vector per text, the model id.
- **Settings:**
  - the new keys load and round-trip;
  - each validation error names its key;
  - `llama-cpp` still requires the model keys;
  - `SettingsEdit` reads and writes the new keys;
  - the engine host rebuilds on a URL change.
- **`grammar::Check`:** as in section 3.
- **Callers:** the app's backend factory picks the right backend for each setting (tested through `AppController` with a factory that records which kind it was asked for).

### `[server]` suite (skipped unless `MORPHEUS_TEST_SERVER_URL` is set)

The owner points `MORPHEUS_TEST_SERVER_URL` (chat) and `MORPHEUS_TEST_EMBED_SERVER_URL` (embedding) at a running `llama-server`, started with at least three parallel slots and a context per slot of at least `context_size`.

- Token counts agree with the in-process backend for the same model, when both are available.
- A turn's resolve and narrate calls complete with a grammar, and thinking works on a thinking model.
- **R2's acceptance:**
  - three sessions on three slots alternate an action call and a speech call, ten exchanges each, on a 16k context;
  - the test reports each later call's reused fraction (`tokensReused / promptTokens`) and asserts it is above zero;
  - the measured figures, especially with Qwen3.5, go into the README.

  The 80% figure is not asserted, because whether Qwen3.5's linear-attention layers allow it through `llama-server` is exactly what is being measured.

### Model-backed suite (`[llama]`)

The agreement test between `grammar::Check` and `LlamaCppBackend::CheckGrammar` (section 3).

## Documentation

- **Architecture spec:**
  - the backend split;
  - the `LlamaServerBackend` module and its row;
  - `grammar::Check`;
  - the settings keys;
  - `SamplingFrom` in `turn`;
  - the slot field.
- **README:**
  - choosing the backend;
  - starting `llama-server` for Morpheus (the flags: `--parallel`, the context per slot, `--embedding` for the embedding server);
  - the `[server]` tests;
  - the reuse measurement once the owner has run it.
- **CLAUDE.md:** the layout list gains `src/llamaserver`.

## Out of scope

- `morpheus_server` itself and choosing a slot per conversation: sub-project D.
- HTTPS and authentication.
- Bumping llama.cpp's pinned version: not needed for this work.
