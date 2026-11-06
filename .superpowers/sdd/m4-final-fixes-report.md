# Milestone 4 final-review fix round: report

Status: DONE. Commits: 21cc1b7 "Harden the app model after the milestone 4 review", 5db94f1 "Bring the docs in line with the milestone 4 build". Working tree clean afterwards.

## Items (commit A)

1. SaveSettingsEdit (src/app/model/SettingsEdit.cpp/.h)
   - The JSON patch is wrapped in `catch (nlohmann::json::exception)` -> DataError naming the settings file.
   - rename uses the error_code overload -> DataError("cannot replace <file>: <reason>").
   - The write/check/rename block is wrapped so the ".new" file is removed on every failure path.
   - A LoadSettings DataError is rethrown with the check file's PathText replaced by the settings file's PathText.
   - Header comment updated to match.
   - Tests added/changed in SettingsEditTests.cpp: the invalid-model test now also requires "settings.json" in the message and no "settings.json.new"; new misshapen test (`"sampling": []` and a top-level array: DataError naming settings.json, file unchanged, no .new); new "cannot replace" test (an open std::ifstream holds settings.json without delete sharing, so Windows refuses the rename).
2. Catalog::RunNameProblem rejects con, prn, aux, nul, com1-9, lpt1-9 case-insensitively ("... is a reserved device name on Windows"). Header comment updated; the "safe on every system" comment is now true. Test: CON/NUL/Com9/etc. rejected; console, com10, com0, nul2 accepted.
3. RunModel: Apply requires Opening for RunOpened and Undo for StateChanged; OnPosted accepts Undo only for an explicit UndoTurn (REQUIRE holds_alternative); _lastError is cleared only after the REQUIREs pass (per branch, right before the state change); `#include <cstddef>` added. Tests: RunOpened during a turn and StateChanged while opening throw ContractError; a rejected UndoTurn keeps the previous LastError. (The "unknown command" REQUIRE cannot be reached by a test: Command has exactly five alternatives, all handled earlier.)
4. AppController constructor loads settings, lists worlds, then starts the engine. Test: missing projectsDir throws DataError and the factory was never called.
5. Test only (tests/app/AppControllerTests.cpp): factory succeeds, throws once (InferenceError), succeeds. Run opened and one turn played; a gpu_layers edit -> engine Failed, CanPost false, Run not open, OpenRun empty; a second working edit -> run reopens with 1 turn in history, OpenRun set to the same run file, factory called 3 times. It passed on first run (no production change needed; the behavior already held).
6. src/turn/Worker.h: MemoryView moved above the "Events (worker to UI)" comment block, so that block sits directly on the event structs again.
7. src/app/main.cpp: throws std::runtime_error("cannot load font file <path>") when AddFontFromFileTTF returns nullptr.

## TDD evidence

Tests were written first, then `cmake --build --preset tests` and the tags run (RED, before any production change):
- `./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit]"`: test cases 6 | 3 passed | 3 failed, assertions 37 | 29 passed | 8 failed. Failures: message contained "settings.json.new"; rename threw std::filesystem_error ("rename: unknown error: ...") instead of DataError and left settings.json.new behind; misshapen file threw a nlohmann type_error instead of DataError.
- `"[catalog]"`: test cases 5 | 4 passed | 1 failed, assertions 43 | 33 passed | 10 failed (reserved names returned "").
- `"[run_model]"`: test cases 9 | 7 passed | 2 failed, assertions 57 | 54 passed | 3 failed (RunOpened/StateChanged accepted with no exception; LastError cleared to "" by the rejected post).
- `"[app_controller]"`: test cases 7 | 6 passed | 1 failed: `CHECK( made == 0 )` with expansion `1 == 0` (the engine started before the worlds folder was checked). The reopen test (item 5) was GREEN already.

GREEN after the fixes: [settings_edit] 37 assertions in 6 test cases; [catalog] 43 in 5; [run_model] 57 in 9; [app_controller] 130 in 7; [worker] 143 in 10; all passed.

## Verification

- `ctest --preset tests`: 100% tests passed out of 241 (16 s).
- `cmake --build --preset asan && ctest --preset asan`: 100% tests passed out of 241 (51 s).
- `cmake --build --preset dev --config RelWithDebInfo --target morpheus` built; `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 60` printed only llama.cpp model-load notes and `exit 0`.
- Git status was clean after both commits (dotnet/, models/, settings.json, imgui.ini not staged).

## Commit B (docs)

- CLAUDE.md fast-loop line: no llama.cpp or SDL3, ImGui's core is built, tests draw the app's windows headless.
- Architecture spec: Build paragraph names upstream's docking branch (v1.92.8-docking) and says AddressSanitizer is opt-in via `asan` and `dev-asan`; the app row in the module table now depends on "turn, llama (`main.cpp`'s backend factory)".
- README.md: "the same path the app takes"; Settings bullet's reload list names the embedding prefixes.

## Files changed

Commit A: src/app/main.cpp, src/app/model/{AppController.cpp, Catalog.cpp, Catalog.h, RunModel.cpp, SettingsEdit.cpp, SettingsEdit.h}, src/turn/Worker.h, tests/app/{AppControllerTests.cpp, CatalogTests.cpp, RunModelTests.cpp, SettingsEditTests.cpp}.
Commit B: CLAUDE.md, README.md, docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md.

## Concerns

- The "cannot replace" test relies on Windows semantics (an open ifstream blocks rename onto the file). The project is Windows-only, so acceptable.
- A "cannot write <name>.new" message (write failure of the check file) still names the check file, since that is the path that could not be written; the header was worded accordingly ("naming the settings file" for the other failure paths).
- RunModel::Apply mutates _busy/_pending before the new REQUIREs throw (same as the existing TurnCompleted checks); a ContractError is an engine bug, so the model state afterwards is not meant to be used.
