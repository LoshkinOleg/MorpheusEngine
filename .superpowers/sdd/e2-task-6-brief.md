### Task 6: Live run and documentation

**Files:**
- Modify: `tests/llama/LlamaHostedTests.cpp`, `tests/server/ServerLiveTests.cpp`
- Modify: `README.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`

**Interfaces:**
- Consumes: everything above.
- Produces: documentation; two live tests that skip without their variables.

- [ ] **Step 1: The live tests**

Add a `[llama][hosted][rewind]` test to `tests/llama/LlamaHostedTests.cpp` that plays `reload.json` with the shared real backend and `echoedMemories` false (as E1's three-actor live test does; reuse its settings helper), and a `[server][rewind]` test to `tests/server/ServerLiveTests.cpp` that plays it over HTTP against the owner's llama-servers with `echoedMemories` false (as E1's server memory test does). With a real model, the checks are the rewind counts, `turns`, `conversation` and memory `count` and `absent`.

With `echoedMemories` false both runners already skip memories `contains` and check `absent` and `count`, as E1's live three-actor tests rely on; the new `rewind`, `turns` and `conversation` steps never depend on the model's words.

Run: `cmake --build --preset tests` and `ctest --preset tests` (the live tests skip). Build `dev`'s `morpheus_tests` (`cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`) so the `[llama]` file compiles there. Do not start llama-servers; the controller runs the live tests.

- [ ] **Step 2: Documentation**

- README, morpheus_server section: the rewind route (call it on every save load with the save's game time; what it hides; roll-forward until new play; playing on abandons), and the host data's `clock`.
- The architecture spec: the store's tables (`conversation_changes`, `rollbacks`, the new columns), version 6, and one paragraph on rollbacks in "Hosted runs".
- The per-speaker memory spec: superseded summaries are linked to their condenses (`summary_supersessions`), and a rollback can hide memories; one line each, pointing to the rollback spec.
- The morpheus_server spec: the route in its endpoint table, marked "added in 1.2.0 (E2)".
- Byte checks on every changed doc.

- [ ] **Step 3: Commit**

```bash
git add tests/llama/LlamaHostedTests.cpp tests/server/ServerLiveTests.cpp tests/hosted/ README.md docs/superpowers/specs/
git commit -m "Rolling a run back: live runs of the reload fixture, and the docs"
```

---

## Final verification (controller)

- `ctest --preset tests` and `ctest --preset asan`: all pass.
- `cmake --build build/dev --config RelWithDebInfo`: clean; with the models configured, `"[llama][hosted]"` passes.
- With llama-servers running (Qwen3.5-9B and nomic-embed): `"[server]"` passes, including the reload fixture over HTTP.
- The consumer check: ok. Nothing left running.
