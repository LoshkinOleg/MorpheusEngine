### Spec Compliance
- Spec compliant. SettingsEdit fields, Read/Save semantics, window combo, per-backend field visibility, tooltips, help text, the three SettingsEdit tests and the UI smoke test all match the brief. URLs in code, comments and tests use 127.0.0.1 (no `localhost` in the diff).
- The earlier-noted unconditional read/write of `chat_model` and `gpu_layers` is resolved: SettingsEdit.cpp:59-67 (read) and :106-115 (save) are now conditional on the backend.
- Cannot verify from diff: the report's counts (421 tests, asan run, byte checks). Not re-run, per instructions.
- Checked (named risk: EngineHost keeps the old backend after a URL/slot/backend change): src/app/model/EngineHost.cpp:19-23 `sameBackend` does not compare backend, URLs or slot. This is Task 9 per the plan (spec line 213, plan Task 9 files list EngineHost), so not a Task 8 finding; the window's help text ("reload the backend") is only true after Task 9.

### Strengths
- Save patches only the active backend's keys and leaves the other's in place; tested in both directions (SettingsEditTests.cpp round trip) and the refusal test proves the file is untouched on a loader rejection.
- Read of a llama-server file without model keys is tested, including that Save writes no model keys.
- Window hides the inactive backend's fields and has tooltips that explain the slot and context-size relation; layout of SettingsEdit.h groups fields by backend with clear comments.
- Bad `backend` value throws DataError naming the file (fail fast, no fallback).

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
1. SettingsEdit.cpp:59-79: the read goes beyond the loader's contract. Core/Settings.cpp:140-141 says the other backend's keys are "ignored (not read or checked)", but ReadSettingsEdit reads them when present, so a file the engine loads fine (e.g. under llama-cpp, a `server` object with a non-string `chat_url`, or a non-object `server`) makes the Settings window throw DataError. The brief mandates reading them, so this is plan-mandated; consequence is an edge case only on hand-broken ignored keys.
2. SettingsEdit.cpp:201-206: the backend error message omits the offending value, unlike the loader's (Settings.cpp:145). Duplicates the loader's validation; harmless.
3. SettingsWindow.cpp:306-314: three consecutive `if` tests on `edit.backend` (context-size tooltip, then GPU layers) could be one if/else; the context-size tooltip being separated from its InputInt by a condition is easy to misread.
4. Tests: no test for ReadSettingsEdit throwing on an invalid `backend` value or on a llama-server file lacking `server`; the UI smoke test (UiTests.cpp:446-467) asserts only the engine's loaded settings, not the window's output (plan-exempt from TDD, window state is private).
5. SettingsEdit.h:177-183: the SaveSettingsEdit comment has a line well over the 120-column wrap (the "The new text is first written ..." line); reflow.

### Assessment
**Task quality:** Approved
**Reasoning:** The implementation matches the brief, resolves the unconditional model-key read/write, and is covered by meaningful round-trip and refusal tests; only minor polish remains. The EngineHost keep-backend rule is Task 9's work.
