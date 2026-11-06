# Milestone 4 final-review fix round

Two commits, in this order. TDD for every behavior change (test first, show it failing). Follow CLAUDE.md.

## Commit A: "Harden the app model after the milestone 4 review"

1. (Important) SaveSettingsEdit (src/app/model/SettingsEdit.cpp) must throw only core::DataError, as its header promises, so AppController::ApplySettings returns a refusal instead of an exception escaping the frame:
   - Wrap the JSON patch in `catch (const nlohmann::json::exception&)` and throw DataError naming the settings file (a valid but misshapen file, e.g. `"sampling": []` or a top-level array).
   - Turn a failing `std::filesystem::rename` into DataError("cannot replace <file>: <reason>"); use the error_code overload.
   - Remove the check file (".new") on every failure path, not only loader failures.
   - When core::LoadSettings rejects the check file, rethrow a DataError whose message names the real settings file instead of "<name>.new" (replace the check file's PathText with the settings file's PathText in the message).
   - Tests in tests/app/SettingsEditTests.cpp: `"sampling": []` -> DataError naming settings.json, file unchanged, no .new left; the invalid-model-path test now expects the message to name settings.json and not settings.json.new. The existing "does not exist" check stays.
2. Catalog::RunNameProblem (src/app/model/Catalog.cpp) rejects Windows reserved device names case-insensitively: con, prn, aux, nul, com1-com9, lpt1-lpt9. Its comment must be true. Test in CatalogTests.cpp (e.g. "con", "NUL", "com1" rejected; "console", "com10" accepted).
3. RunModel (src/app/model/RunModel.cpp) enforces its documented contract:
   - Apply: RunOpened requires the finished command to be Opening, StateChanged requires Undo (MORPHEUS_REQUIRE).
   - OnPosted: Undo only for an explicit `turn::UndoTurn` (holds_alternative); anything else is a ContractError.
   - OnPosted: clear _lastError only after the REQUIREs pass.
   - Add `#include <cstddef>`.
   - Tests in RunModelTests.cpp: RunOpened while a turn runs, and StateChanged while opening, throw ContractError; a rejected OnPosted keeps the previous LastError.
4. AppController's constructor (src/app/model/AppController.cpp) lists the worlds before starting the engine, so a missing worlds folder fails before any model load starts (otherwise ~EngineHost waits for the load before the error shows). Add a test: a missing projectsDir throws DataError and the factory was never called.
5. Test the reopen across a failed restart (tests/app/AppControllerTests.cpp): a factory that succeeds, then throws once, then succeeds. Open a run, apply an edit that changes a backend setting (e.g. gpu_layers) -> engine Failed, CanPost false, Run not open; apply a working edit -> the run reopens with its history and OpenRun() is set again.
6. src/turn/Worker.h: move the MemoryView struct above the event-contract comment block (or separate them) so that comment documents the events again.
7. src/app/main.cpp: throw std::runtime_error naming the font file when AddFontFromFileTTF returns nullptr.

Run: `cmake --build --preset tests`, the affected tags ([settings_edit], [catalog], [run_model], [app_controller], [worker]), `ctest --preset tests`, then `cmake --build --preset asan && ctest --preset asan`. Build `morpheus` in the dev preset (RelWithDebInfo) for the main.cpp change and run `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 60; echo "exit $?"` from the repo root (opens a window briefly; must exit 0).

## Commit B: "Bring the docs in line with the milestone 4 build"

- CLAUDE.md, the fast-loop line: the tests preset builds no llama.cpp or SDL3 but does build ImGui's core, and the tests draw the app's windows headless.
- docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md:
  - the Build paragraph: ImGui is upstream's docking branch (v1.92.8-docking), not "the ImGui fork"; AddressSanitizer is the opt-in `asan` and `dev-asan` presets, not every Debug build;
  - the module table's app row: depends on turn, plus llama (main.cpp's backend factory).
- README.md: "the same path the app will take" -> "the same path the app takes"; the Settings bullet's reload list also names the embedding prefixes (they reload the models too, see EngineHost::Start).

ASCII only. Never stage dotnet/, models/, settings.json, imgui.ini.
