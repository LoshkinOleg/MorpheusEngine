# Task 12 report: milestone 4 verification

Status: DONE. Branch cpp, HEAD 31c92a7. No source changes, nothing staged or committed (git status clean after all steps).

## Step 1: fast presets
- `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`: exit 0, 235/235 passed (ctest 15.6 s, whole command about 22 s). 0 compiler warnings.
- `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`: exit 0, 235/235 passed (ctest 47.7 s, whole command about 54 s). 0 warnings.

## Step 2: dev preset with models
- Both MORPHEUS_TEST_* variables exported in the same shell command, then `cmake --build --preset dev && ctest --preset dev`: exit 0, 256/256 passed (ctest 224 s, whole command 6 m 26 s). 0 warnings.
- `[llama]` tests: running `morpheus_tests.exe "[llama]"` directly with the variables set (build/dev/bin/Debug): 21 test cases, all passed (86 assertions), none skipped. (The "failed to parse grammar" line in that output is an expected negative test, the run passes.)

## Step 3: dev-asan
- `cmake --preset dev-asan && cmake --build --preset dev-asan`: exit 0 (3 m 35 s). Warnings: only LNK4300 (ignoring /INCREMENTAL because of ASAN metadata) for morpheus_tests, morpheus_play, morpheus_probe, plus the llama.cpp "ccache not found" configure notice. No compiler warnings.
- `./build/dev-asan/bin/RelWithDebInfo/morpheus_tests.exe "[ui],[app_controller],[engine_host],[run_model],[catalog],[settings_edit],[inspect],[imgui]"`: All tests passed, 420 assertions in 35 test cases, 4 s, no ASan reports.
- Note: the `[imgui]` tests print `[imgui-error] In window 'Debug##Default': Calling End() too many times!` to the console; the run still passes (it comes from the ImGui error-handling test, not a failure).

## Step 4: app smoke run
- `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 600; echo "exit $?"`: exit 0, 10.7 s wall. Only llama.cpp model-load notices on stderr (the `</s>` control-token note, n_ctx_seq 16384 < n_ctx_train, special_eos_id note), all from the model files, not errors.
- Also run with the Debug build (what `--preset dev` builds): `./build/dev/bin/Debug/morpheus.exe --frames 600`: exit 0, 12.2 s. Both runs came out the same.
- `cmake --build --preset dev --config RelWithDebInfo --target morpheus` reports up to date (the RelWithDebInfo exe is current).
- No hang; no taskkill needed.

## Fixes made
None.

## Owner's interactive checklist (a subagent cannot drive the window)
1. Start a sandcrawler run and play three turns.
2. Watch the narration stream, and the rolls under each input.
3. Regenerate, then undo.
4. Change the narrate temperature and Apply. The run reopens without reloading the models.
5. Quit, relaunch, and Continue the run.
