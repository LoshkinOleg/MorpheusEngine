# Final review: sub-project C, model access over HTTP (1d71fbf..c7e889f, 17 commits on master)

Reviewer: final whole-branch review (read-only on the checkout). Inputs: the spec
(docs/superpowers/specs/2026-11-15-llama-server-backend-design.md), the plan and its Global Constraints, the whole
diff, the current sources, the progress ledger's sub-project C section, and cpp-httplib v0.59.0's own code
(build/dev/_deps/morpheus_cpp_httplib-src/httplib.h) for the stop, send and retry paths.

Checked here, on c7e889f: `cmake --build --preset tests` and `ctest --preset tests` pass (exit 0; the three `[server]`
tests skip without servers). The byte check over every file the range touches prints nothing. No forbidden path
(settings.json, models/, imgui.ini, .superpowers/, docs/references/) is in any commit.

## Strengths

- **The interface split is clean and complete.** `Tokenizer`, `ChatModel`, `Embedder` and `LlmBackend` as their union
  (src/inference/Llm.h:113-161), `GenerationSpec::slot` (Llm.h:53-55), `SamplingFrom` moved to turn
  (src/turn/Session.h:43), `CheckGrammar` off the interface and kept as a plain `LlamaCppBackend` method. Every fake,
  `ModelSeatBackend` and `RecordingBackend` follow it. `morpheus_inference` links nothing.
- **Shared rendering, no copies.** `ContentMarker`, `MarkContents`, `TemplatePieces` and the thinking texts live in
  `inference`, and both backends use them (LlamaCppBackend.cpp:663-691, LlamaServerBackend.cpp:583-631), so contents
  never become control tokens on either path. The `[server][llama]` agreement test checks `CountPromptTokens` as well
  as `CountTokens`.
- **The module dependency rule holds.** `<httplib.h>` is included only by src/llamaserver/LlamaServerBackend.cpp,
  tests/fakes/FakeLlamaServer.h and tests/llama/LlamaServerAgreementTests.cpp, each behind the guarded
  `WIN32_LEAN_AND_MEAN`. `<llama.h>` is still only in src/llama. `morpheus_llamaserver` links inference PUBLIC and core,
  json, httplib PRIVATE. The private `morpheus_httplib` target never meets llama.cpp's vendored cpp-httplib (0.48.0):
  with `LLAMA_BUILD_COMMON`, `LLAMA_BUILD_SERVER` and `LLAMA_BUILD_TOOLS` off, llama.cpp builds no httplib library, so
  the app, play and probe hold one copy only.
- **The error categories are applied with care.** Bad URLs, unreachable servers, HTTP errors, malformed answers,
  small slots, more than one special token and a server count that disagrees with the sent prompt are
  `InferenceError` naming the URL and path. Caller mistakes (`maxTokens <= 0`, a too-small thinking budget, a negative
  slot) are `ContractError`. Malformed settings are `DataError` naming the key. The URL is parsed before any client is
  made, so a bad port fails loudly. Before, it would have made cpp-httplib's unusable client.
- **Cancel is sound against cpp-httplib's real behaviour.** The `std::stop_callback` calls `Client::stop()` on the
  dedicated stream client only. In v0.59.0 that shuts down a socket in flight under `socket_mutex_`, or disconnects an
  idle one. `upload_progress` catches a stop that lands before the body is written. The retry is barred once a stop is
  requested. The stop_callback's destructor waits for a running callback, so the client never outlives its use. Both
  the mid-stream and the silent-server cases are tested (LlamaServerCompleteTests.cpp:388, 477).
- **The retry logic is narrow and tested both ways.** `Ask` retries once only on Read/Write over a reused socket. A
  /completion retries only with no status, no receiver failure and no stop. Tests cover one retry over a new
  connection, no retry on a fresh connection, and no retry once an answer has begun. Re-sending the same
  `httplib::Request` is safe: `write_request` adds Host and Content-Length only when they are missing.
- **The settings round trip is consistent.** The loader reads only the keys of the backend in use and leaves the
  others' at their defaults. `SettingsToJson` writes only the backend in use. `SaveSettingsEdit` writes the chosen
  backend's keys and leaves the other's as the file has them, then validates through the real loader before it
  replaces the file. Because the loader leaves the unused kind's fields default, `EngineHost`'s keep-the-backend rule
  (EngineHost.cpp:19-26) does not reload spuriously in practice.
- **The vendored build is checked.** The consumer check requires `Morpheus::llamaserver` and `morpheus_httplib` and
  still forbids llama.cpp, SDL3, ImGui and Catch2.
- **Live evidence covers what the fast suite cannot.** That is token agreement, thinking in two calls on Qwen3.5, and
  R2's three-slot reuse. The README records it, including the `--checkpoint-min-step 0` finding.

## Issues

### Critical (must fix)

None.

### Important (should fix)

1. **The model seat's Check now makes an HTTP request on the UI thread, and a failed request ends the app.**
   - File: src/app/model/ModelSeat.cpp:129 (called from src/app/ui/ModelSeatWindow.cpp:184; caught only by
     src/app/main.cpp:259).
   - What is wrong: `ModelSeat::Check` calls `_pBackend->CountTokens(text)` on the UI thread while it holds the seat's
     mutex. Before this branch that was an in-process call that could not fail in practice. Under `llama-server` it is
     `POST /tokenize`, and it throws `InferenceError` when the chat server is down or restarting. Nothing on the UI
     path catches it, so the exception reaches main's catch-all: the app shows a message box and exits, and the reply
     the owner typed is lost. A hung server also freezes the UI for up to two 60 s read timeouts (`Ask` retries once
     over a reused socket), with the seat mutex held all that time.
   - Why it matters: this is the one place where the backend runs on the UI thread (architecture spec, "Engine worker
     thread"). The branch turned it from infallible into fallible without changing how failure is handled. On the
     worker, the same `InferenceError` becomes an `Error` event and the app carries on.
   - How to fix: catch `inference::InferenceError` around the token count in `ModelSeat::Check` and return it in
     `SeatCheck`, for example as the problem "cannot count tokens: <what>" with no token count, so that the window
     shows it. Test it first with a backend whose `CountTokens` throws. One sentence in the architecture spec's thread
     paragraph should note that under llama-server Check makes a blocking HTTP request (up to the read timeout). Moving
     the count off the UI thread is not needed for a debug window.

### Minor

Findings of this review (not in the ledger):

- **M1. The interface promises a ContractError that one implementation does not throw** (src/inference/Llm.h:134-137).
  `ChatModel::Complete` says it throws `ContractError` for "a grammar that does not parse". `LlamaServerBackend` sends
  the grammar to the server, which refuses it with HTTP 400, so the caller gets an `InferenceError` and the worker
  shows "error: ..." where in-process shows "engine bug: ...". It still fails loudly, and Morpheus's grammars come from
  the builder. Fix the comment: in-process throws `ContractError`, and llama-server reports the server's refusal as
  `InferenceError`.
- **M2. Run meta `model_id` is empty under llama-server** (src/turn/Session.cpp:84). `meta.modelId` is
  `settings.chatModel.stem()`, and the loader leaves `chatModel` empty under llama-server. Every run started through
  the server records `model_id ""`. Only tests read it today; `meta.settings` still holds the server URLs. The proper
  fix needs a chat-model id from the backend, which the chat server's `/props` `model_path` gives: something like a
  `ChatModel::ModelId()`. That belongs with sub-project D; record it as a follow-up.
- **M3. "Reused" means the socket handle is open, not that the server kept the connection**
  (LlamaServerBackend.cpp:96, 234). When llama-server has closed an idle socket, cpp-httplib's `send_` notices and
  reconnects silently, but `is_socket_open()` was already true. A Read/Write failure on that fresh connection is
  therefore retried, which the comments at 88-94 and 229-233 say never happens. This is harmless: one extra attempt,
  and the connect fails fast when the server is gone. Widen the comments, or leave it.
- **M4. A lore embed's transport failure is reported as a world data problem** (src/turn/Retrieval.cpp:281-285). Any
  `InferenceError` from embedding a lore entry becomes "lore.csv entry 'x' cannot be embedded: ...". Under HTTP that
  includes "cannot reach .../embedding". The cause stays in the message, and `DataError` is only shown, so this can
  stay.
- **M5. A kept backend's cached `/props` facts go stale when a server's model changes under the same URL.** The cached
  facts are the thinking markers, the BOS id and the embedding model id. Apply with unchanged settings keeps the
  backend (EngineHost.cpp:19-33). New vectors would then be stored under the old embedding model id, so stored
  embeddings no longer match the model that made them, silently. This needs the owner to swap models while the app
  runs. Suggested: one README sentence ("after restarting a server with another model, restart the app or change the
  URL"). A guard is not needed now.
- **M6. The slot is not checked against the server's slot count at startup.** `/props` has `total_slots`, but a slot
  of `--parallel` or above fails only at the first `Complete`, with the server's message. It is still loud. A
  constructor check would fit the fail-fast rule; optional.
- **M7. settings.example.json has no `server` block.** Switching to llama-server in the Settings window starts from
  empty URLs, and the first Apply is refused. A `server` block with the 127.0.0.1 example URLs (ignored under
  llama-cpp) would start the switch from working values. Optional.

## Triage of the ledger's deferred Minors

Fix before push (all small, comment or doc only):

- T6: the stop-during-connect comment in `StreamCompletion` is wrong (LlamaServerBackend.cpp:142-145). In v0.59.0,
  `send_` raises `socket_requests_in_flight_` under the same `socket_mutex_` it holds while connecting. A stop that
  waits out the connect therefore finds the request in flight and shuts its socket down: the write fails, the retry
  is barred, and the call is cancelled. It does not "find no request in flight". Comments that misdescribe threading
  mislead the next change.
- T6: the retry wording (LlamaServerBackend.cpp:229-233, header :37-39). llama-server sends the /completion headers
  only after the first token, so "no status" covers the whole prompt read. A reused-socket failure or read timeout
  while a long prompt is read is retried, and a hung server takes about 2 x 600 s to fail. Say so in the comment.
  Fold M3's wording into the same edit.
- T11: in the architecture spec's module table, the `turn` row says "all above"
  (docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md:35). Above it are now `llama` and
  `llamaserver`, which turn does not link. Write turn's real dependencies: core, store, inference, context, grammar.
  This table documents the dependency rule the branch extends.
- T11: the architecture spec's settings bullet (same file, :134) says `n_ctx`; the key is `context_size`.
- M1 (new, above): the `Complete` grammar-error contract line in Llm.h.

Can stay (record in follow-ups where noted):

- T1: some `SamplingFrom` call sites could be rewrapped. The interface-shape tests are compile-time only, as the plan
  specified.
- T2: `Check` re-reads the grammar per call and copies memo sets. This costs only in the brute-force `IntRange` test;
  a single Check of a resolve grammar is fast.
- T2: the `< 5 s` wall-clock assertion also runs under asan. It passes there today; watch for flakes.
- T2: there is no exact-reason test for a bounded-repeat overrun or a partial literal in a referenced rule. Follow-up.
- T2: a backslash-U literal can match the NOT_UTF8 sentinel. No Morpheus grammar writes one. Follow-up for grammar
  hardening.
- T2: the "does not start a UTF-8 character" comment wording.
- T3: the window's Check-button test no longer proves the press reaches the checker (plan-mandated). Revisit together
  with Important 1's test.
- T4: the fake has no test that a timed-out held stream leaves an earlier close flag set. Its setters are exercised by
  later tasks.
- T5: a model that adds both BOS and EOS is refused, and IPv6 literal hosts are refused. Both are documented
  (header, README).
- T5: the helper lambda's name, and an overlong fake comment line.
- T6: cancelled calls report less telemetry than in-process. The session discards cancelled completions, so nothing
  reads it.
- T6: the re-tokenized thinking may push the answer call's prompt plus `maxTokens` a few tokens past `ContextSize()`
  unchecked (LlamaServerBackend.cpp:479-495). The server's per-slot context is at least `context_size`, and an
  overrun ends as a server error or a "limit" stop, not silently. Throwing here would turn working turns into errors.
- T6: the fake writes headers before it goes silent, unlike llama-server. The stop test still passes for the right
  reason, the stop closing the socket.
- T6: a comment reflow (also LlamaServerBackend.h:70-71).
- T7: the loader's URL check accepts `http:///x` and `http://:8080`. The backend refuses both at load, naming the URL,
  so the engine shows Failed. The overlong line at Settings.cpp:223. No test for a non-integer or above-INT_MAX slot:
  `requireInt` is shared and tested elsewhere.
- T8: SettingsEdit reads the other backend's keys when present, so a malformed ignored key breaks the window but not
  the engine (brief-mandated, and loud). Its backend error omits the value (SettingsEdit.cpp:50), while the loader's
  includes it. The three `if`s on the backend in SettingsWindow. No SettingsEdit tests for an invalid backend or for
  llama-server without `server`; the loader tests cover both. The SettingsEdit.h comment is over the wrap width.
- T9: the server and llama.cpp config blocks are repeated in main.cpp, play and probe (brief-mandated; one shared
  helper would need a home that links both backends).
- T9: `sameBackend` "reloads on the other kind's fields": moot, because the loader leaves them default. Play prints no
  server line, unlike probe.
- T10: the thinking test can flake if the model ends inside the thinking block. `reused > 0` on every call may fail on
  a hybrid model (spec-mandated). The action and speech prompts diverge after the player's line, which is the test's
  design and is described in the README figures.
- T10: the agreement test copies its loop variable by value, has an unneeded build dependency, and compares
  `filename()` across OSes.
- T10, optional: `REQUIRE(log.reused.size() == ...)` at LlamaServerLiveTests.cpp:204 aborts at the first short
  session and hides the other sessions' figures. CHECK, plus a `continue` for that session, keeps the diagnostics. It
  only matters on a failing live run.
- New M2 to M7: as above. M2 goes into the follow-ups for sub-project D. M5 and M7 are one-line doc or example
  changes, worth doing if convenient.

## Recommendations

- Treat "the backend is fallible and may block" as the new default when a UI path reaches the backend. Important 1 is
  the only such path today. Sub-project D's server will add more callers of `Tokenizer` alone.
- When sub-project D adds a chat-model id to the interface (M2), the stale-cache question (M5) can be answered in the
  same place: compare the server's `/props` model on `EngineHost::Start` when the backend is kept.

## Assessment

**Ready to push:** With fixes.

**Reasoning:** The backend, the interface split, the retry and cancel paths, the settings round trip and the vendored
build are sound and well tested, and the live runs confirm the behaviour on real servers. One Important finding
(Important 1: Check on the UI thread can end the app under llama-server) and four comment or doc corrections should
land before push. None of them changes the design.
