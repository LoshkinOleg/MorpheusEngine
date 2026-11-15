### Task 8: Live runs and documentation

**Files:**
- Modify: `tests/hosted/HostedTestSupport.h` (the fixture runner takes its backend)
- Modify: `tests/llama/LlamaHostedTests.cpp`, `tests/server/ServerLiveTests.cpp`
- Modify: `README.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, `CLAUDE.md` (only if a layout line changes)

**Interfaces:**
- Consumes: everything above.
- Produces: `test::PlayMemoryFixture(const std::string& name, inference::LlmBackend& backend, const core::Settings& settings, bool echoedMemories)`; the old one-argument form becomes a wrapper that makes the echo backend and calls it with `echoedMemories = true`.

- [ ] **Step 1: The runner takes its backend**

Split `PlayMemoryFixture` (Task 5, extended in Task 6): the four-argument form plays the fixture on `backend` with `settings`; with `echoedMemories` false (a real model writes the memories), it checks only what cannot depend on the model's words: every `prompt_absent`, every memories `absent` and `count`, the share's `pending`, and that each turn commits; it skips `prompt_contains`, memories `contains`, `told_by` and `shared`. The one-argument form keeps today's behaviour:

```cpp
inline void PlayMemoryFixture(const std::string& name)
{
    HostedScript script = ScriptOf(ReadFixture(name)); // The fixture's model replies, in step order.
    ScriptedBackend backend(16384, MemoryEchoResponder(script));
    PlayMemoryFixture(name, backend, HostedSettings(), true);
}
```

(`ScriptOf` is the loop that fills `script.actions` and `script.lines` from the steps, now a helper with two callers: this wrapper and the HTTP test, which should use it too.)

With a real model, a turn's line is the model's, so the fixture's `model` replies are ignored, and the substrings checked as absent (`"takes bribes"`, `"back to his shop"`) could appear only if a model wrote them unprompted where its NPC never heard them: a failure there is a real leak or a hallucination, and the test says which turn.

- [ ] **Step 2: The live tests**

In `tests/llama/LlamaHostedTests.cpp` (read it first: it has the in-process backend setup and skips without `MORPHEUS_TEST_CHAT_MODEL` / `MORPHEUS_TEST_EMBED_MODEL`), add:

```cpp
TEST_CASE("The three-actor fixture on the real model: nobody remembers what it did not witness", "[llama][hosted][memory]")
{
    llama::LlamaCppBackend& backend = test::SharedLlamaBackend(); // Use the file's own accessor for the shared backend.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    test::PlayMemoryFixture("three_actors.json", backend, settings, false);
}
```

(adapt the backend accessor to the file's helper). In `tests/server/ServerLiveTests.cpp`, add a `[server]` test that plays the same fixture over HTTP against the owner's llama-servers, like the HTTP test of Task 7 but on `LlamaServerBackend`s (the live test there shows how) and checking only what `echoedMemories = false` checks; extract the HTTP fixture loop of Task 7 into a helper in `tests/server/ServerTestSupport.h` taking the same flag so both tests share it.

Run: `cmake --build --preset tests`, then the fast suite (`ctest --preset tests`): the live tests skip without their variables.

If the owner's models are configured (`MORPHEUS_TEST_CHAT_MODEL`, `MORPHEUS_TEST_EMBED_MODEL`), build `dev` (`cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`) and run `"[llama][memory]"`; report the result. Do not start llama-servers yourself; the controller runs the `[server]` one.

- [ ] **Step 3: Documentation**

- README, morpheus_server section: the two routes in the endpoint table and the API version; a short paragraph on memory: each NPC remembers only what it witnessed (its own summaries, in its words, shown in its prompt's tail; retrieval limited to what it saw and was told); sharing; that the player has no memories in Morpheus.
- The architecture spec: in the module table's `turn` row, "per-speaker memory"; the `summaries` table row (`actor_id`, `conversation_id`, `told_by`, `source_id`, `superseded`), new rows for `actor_memory` and `pending_shares`; schema version 5; in "Hosted runs": summaries are per NPC (link this spec), the hosted prompt's memories are in the tail, and `MemoryUpkeep` works through a `SummaryScope`.
- The hosted-world spec: where it describes run-wide summaries (section 1 memory decision, the hosted prompt's L2, "Shared memory upkeep"), add one line each that E1 replaced them with per-NPC summaries, pointing to `2026-11-16-per-speaker-memory-design.md`.
- The morpheus_server spec: the two routes and API 1.1.0 in its endpoint table, marked "added in 1.1.0 (E1)".
- Check every changed doc with the byte check.

- [ ] **Step 4: Commit**

```bash
git add tests/hosted/HostedTestSupport.h tests/llama/LlamaHostedTests.cpp tests/server/ README.md docs/superpowers/specs/
git commit -m "Per-speaker memory: live runs of the three-actor fixture, and the docs"
```

---

## Final verification (controller)

- `ctest --preset tests` and `ctest --preset asan`: all pass; the `[llama]` and `[server]` tests skip without their variables.
- `cmake --build build/dev --config RelWithDebInfo`: clean; with the models configured, `"[llama][hosted]"` passes.
- With llama-servers running (Qwen3.5-9B and nomic-embed, as in the README): `"[server]"` passes, including the three-actor fixture over HTTP.
- The consumer check (README): ok.
- No `morpheus_tests`, `morpheus_server`, `llama-server` or `ctest` process left running.
