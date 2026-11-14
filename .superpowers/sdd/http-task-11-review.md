### Spec Compliance
- Issues found: CLAUDE.md's `src/app` line still says main.cpp builds "the llama.cpp backend" only (see Important 1). Otherwise compliant with the brief and the controller additions.
- Cannot verify from diff: the verification run (426 tests, ASan, `[llama]` 27 passed) is the implementer's report only; not re-run (instructions forbid it). The README "Not measured yet" section is as the brief specifies.

### Strengths
- Every factual claim I checked matches the code: settings keys and validation (Settings.cpp:32-44, 111-168: `backend` required, unknown backend is a DataError naming the key, `server` keys chat_url/embedding_url/slot, `http://` prefix, slot >= 0, each backend's keys ignored under the other); `[server]` tag and both env vars (LiveServer.h:12-26), 16384 per slot / 3 slots (LiveServer.h:18, LlamaServerLiveTests.cpp:140), `[server][llama]` and `MORPHEUS_TEST_CHAT_MODEL` (LlamaServerAgreementTests.cpp:23-38), ten exchanges and "reuses some" (`CHECK(reused > 0.0)`), `tokensReused / promptTokens` (ReusedFraction), `MORPHEUS_BUILD_LLAMA_SERVER` default ON (CMakeLists.txt:27), module dependencies (inference has no links, llamaserver: inference public, core private, grammar -> core), `app::BackendFactoryByKind` (EngineHost.h:59), `turn::SamplingFrom` (Session.h:45), the five THINKING_* constants (Llm.h:85-89), `GenerationSpec::slot` (Llm.h:55), the interface split (Llm.h:114-161), the 24576 = 73728/3 arithmetic against settings.example.json.
- The new behaviour is described as built (LlamaServerBackend.h:30-40, .cpp:85-135, 268-296): one kept connection per server, a dedicated `/completion` client, retry once of non-streaming requests (Read/Write error on a reused socket) and of an unanswered `/completion`, URL validation at construction, IPv6 literals refused (host charset excludes ':' / brackets).
- `127.0.0.1` everywhere; the only `localhost` in README/spec is the explanatory sentence (README.md:91), which gives the reason as required. Grep of `GbnfMatcher`, `check_grammar`, `CheckGrammar` over spec, README, CLAUDE.md, src/: only `LlamaCppBackend::CheckGrammar`'s own definition/comments and the one intended spec bullet remain, all correct (it is still a plain method). The only `localhost` in src is the LlamaServerBackend.h:35 comment explaining the 2 s cost, which is correct.
- ASCII: zero bytes >= 0x80 in CLAUDE.md, README.md, the spec and ModelSeat.h (LC_ALL=C grep). The `_pBackend` comment is accurate: ModelSeat.cpp:129 uses it only for `CountTokens`; the grammar check is `grammar::Check` (ModelSeat.cpp:128).

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
1. CLAUDE.md:35 (src/app bullet): "main.cpp ...: the SDL3 window and the llama.cpp backend." is stale. main.cpp now builds both backends and picks by `settings.backend` (src/app/main.cpp:114-138, 228 `BackendFactoryByKind(MakeLlamaBackend, MakeServerBackend)`); the architecture spec table row was updated to "llama and llamaserver" but CLAUDE.md, the instruction file, was not. Suggested: "the SDL3 window and the backends settings.json names (llama.cpp or llama-server)". This is the kind of stale spot the controller asked to be grepped for; the `GbnfMatcher`/`CheckGrammar` greps could not find it.

#### Minor (Nice to Have)
1. Architecture spec module table, `turn` row: "all above" is now wrong in the same way it already was for `llama`: turn links only core, store, inference, context, grammar (src/turn/CMakeLists.txt:13), not llama or llamaserver, which now sit above it in the table. Pre-existing wording; could read "core, store, inference, context, grammar".
2. Architecture spec line 134 (settings bullet) names `n_ctx`; the settings key is `context_size` (settings.example.json:7, Settings.cpp). Pre-existing, but the sentence was rewritten in this commit, so it was a cheap fix.
3. README.md:87 says the URLs are "no path ... a bad URL fails at startup naming it", while Settings.cpp:159 only checks the `http://` prefix; a path or IPv6 host is refused later, by the backend constructor (InferenceError naming the URL), not at settings load. Accurate in effect (the engine starts the backend at app start) but the Settings window's Apply will accept such a URL and only fail when the backend loads. Documented behaviour is fine; no change required.
4. Spec bullet 133 (LlamaServerBackend) is one very long line mixing connection handling, startup and thinking; splitting the connection sentences into their own bullet would read better. Style only.
5. Implementer's report says the consumer-check CMakeLists needed no change; confirmed (cmake/consumer-check/CMakeLists.txt:17-33 already expects Morpheus::llamaserver wording and morpheus_httplib).

### Assessment
**Task quality:** Needs fixes (one stale CLAUDE.md line)
**Reasoning:** The docs are accurate against the code on every claim I traced, including the post-plan connection, retry and URL-validation behaviour and the 127.0.0.1 requirement, and all files are ASCII. The single Important item is a one-line stale description of main.cpp in CLAUDE.md; fix it and this is approved.
