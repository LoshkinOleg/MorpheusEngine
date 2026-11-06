### Task 12: Milestone verification

This task checks the whole milestone on every preset, the model-backed tests included, and starts the real app once. It writes no new behavior. Fix only what verification finds, each fix with a test that covers it, in commits of its own.

**Files:**
- Modify: only what verification finds.

- [ ] **Step 1: The fast presets**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Then: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: everything passes under both. Record the test counts.

- [ ] **Step 2: The dev preset, with the models**

Run, in one shell command:

```bash
export MORPHEUS_TEST_CHAT_MODEL="C:/Users/user/Desktop/MorpheusEngine/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf" && export MORPHEUS_TEST_EMBED_MODEL="C:/Users/user/Desktop/MorpheusEngine/models/nomic-embed-text-v1.5.Q8_0.gguf" && cmake --build --preset dev && ctest --preset dev
```

Expected: everything passes, the `[llama]` tests included (they run, not skip). Record the count.

- [ ] **Step 3: dev-asan builds**

Run: `cmake --preset dev-asan && cmake --build --preset dev-asan`
Expected: it builds. `dev-asan` has the app off, but its tests now include the app model and UI libraries and ImGui. Running the whole `ctest --preset dev-asan` is slow (CPU llama.cpp under ASan). Run only the app tests there: `./build/dev-asan/bin/RelWithDebInfo/morpheus_tests.exe "[ui],[app_controller],[engine_host],[run_model],[catalog],[settings_edit],[inspect],[imgui]"`.

- [ ] **Step 4: The app on the real models**

From the repository root, run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 600; echo "exit $?"`
Expected: about ten seconds of window, then `exit 0`.

The interactive checks are the owner's: a subagent cannot drive the window. List them in the report for the owner:
1. Start a sandcrawler run and play three turns.
2. Watch the narration stream, and the rolls under each input.
3. Regenerate, then undo.
4. Change the narrate temperature and Apply. The run reopens without reloading the models.
5. Quit, relaunch, and Continue the run.

- [ ] **Step 5: Report**

Report the counts from Steps 1-3, the smoke runs, any fixes made (with their commits), and the owner's checklist from Step 4.

---

## Self-review notes (for the plan's executor)

- **Spec coverage:** the six windows of the spec's UI table are Tasks 9-10 (Play: transcript, streaming prose, input box, undo/regenerate and inline rolls; State: tree with marks; Context: layers, reused vs. evaluated, raw/formatted; Turn log: resolve JSON, rolls, applied and rejected, timings; Memory: summaries, notes, retrieval hits with scores; Settings: model paths, context size, sampling, memory_mode). Docking, threading and the event flow are Tasks 1, 2, 7 and 8.
- **Order:** Tasks 1-8 build and test under `tests` alone. Task 9 needs Task 2's ImGui, Task 11 needs the `dev` preset.
- **Deferred, for the owner:** the `SCHEMA_VERSION` bump, a fallback-summary flag in the store, editing the budget in the app, and retrieval threshold tuning.
