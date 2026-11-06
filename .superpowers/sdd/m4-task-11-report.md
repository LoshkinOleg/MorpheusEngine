# Task 11 report: the app executable

Status: DONE. Commit 31c92a7 "Replace the prototype with the SDL3 and ImGui app".

## Implemented
- src/app/main.cpp created verbatim from the brief (SDL3 window + SDL_Renderer, ImGui docking, Segoe UI, imgui.ini under the root, MakeLlamaBackend factory, Ui::Draw per frame, `--frames N`).
- src/app/CMakeLists.txt: `morpheus` target (morpheus_app_ui, morpheus_llama, imgui_sdl3), `morpheus_target_defaults`, VS_DEBUGGER_WORKING_DIRECTORY = repo root.
- Root CMakeLists.txt: prototype block and MORPHEUS_MODELS_DIR removed; option text changed; `SYSTEM` added to the sdl3 FetchContent_Declare; VS_STARTUP_PROJECT block added.
- `git rm src/main.cpp`; `/imgui.ini` added to .gitignore.
- Docs: README.md (preset row, "Play in the app" section, Layout line), CLAUDE.md (dev line, src/app line, llama module rule), architecture spec (Runs row, paragraph, app row in the modules table).

## Build
`cmake --preset dev` and `cmake --build --preset dev --config RelWithDebInfo --target morpheus`: exit 0, zero warnings in the build log (SDL headers are SYSTEM, /WX satisfied). Output: build/dev/bin/RelWithDebInfo/morpheus.exe (SDL3.dll beside it).

## Smoke runs
- `morpheus.exe --frames 120` from the repo root: exit 0. Real models loaded (llama.cpp load notices printed on stderr about the model's tokenizer; not ours). imgui.ini was written at the root (1314 bytes, has Runs/Settings window entries) and `git status --short` did not list it. I deleted imgui.ini afterwards.
- `morpheus.exe nowhere --frames 1`: prints `error: cannot read 'nowhere/settings.json'`, exit 1, no message box.

## Solution checks
- build/dev/MorpheusEngine.sln: `grep -c morpheus` = 12; the `morpheus` project (src\app\morpheus.vcxproj) is the first real project in the .sln, which is how CMake marks the startup project.
- build/dev/src/app/morpheus.vcxproj: LocalDebuggerWorkingDirectory = C:/Users/user/Desktop/MorpheusEngine for every configuration.

## Tests
- `tests` preset: build with no warnings; ctest 235/235 passed.
- `asan` preset: build clean except the existing linker notice LNK4300 (/INCREMENTAL ignored with ASAN metadata, not an error); ctest 235/235 passed.

## Files changed
CMakeLists.txt, src/app/CMakeLists.txt, src/app/main.cpp (new), src/main.cpp (deleted), .gitignore, README.md, CLAUDE.md, docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md. Nothing else staged (no dotnet/, models/, settings.json, imgui.ini).

## Self-review
- Code, CMake and doc text match the brief verbatim. src/app/CMakeLists.txt briefly had mixed line endings after my append; normalized to LF (the repo's committed form).
- The README's "Play in the app" section was added before "Play in the terminal" as specified; README still says "the same path the app will take" in the terminal section (not in the brief, left untouched).

## Concerns
- The spec's Build paragraph still says "the ImGui fork" (not in the brief's scope; upstream ImGui is used now).
- The Debug configuration of morpheus was not built or run; only RelWithDebInfo.
- Interactive behavior (docking, input, play) was not exercised, only the 120-frame smoke run.
