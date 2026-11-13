# Consumable build: global constraints and decisions

Verbatim from the plan docs/superpowers/plans/2026-11-13-consumable-build.md. Spec: docs/superpowers/specs/2026-11-13-consumable-build-design.md.

## Global Constraints

- **Options (root `CMakeLists.txt`, verbatim from the spec):**
  - `MORPHEUS_BUILD_APP`, default `PROJECT_IS_TOP_LEVEL`: the app (needs llama.cpp; fetches SDL3).
  - `MORPHEUS_BUILD_TESTS`, default `PROJECT_IS_TOP_LEVEL`: `morpheus_tests` (fetches Catch2; ImGui's core for the headless UI tests).
  - `MORPHEUS_BUILD_LLAMA`, default `PROJECT_IS_TOP_LEVEL`: the llama.cpp backend, `morpheus_probe` and `morpheus_play`.
  - `MORPHEUS_GPU_BACKEND`, default `Vulkan`: `Vulkan`, `CUDA` or `CPU`; a cache STRING with those three values as its `STRINGS` property; any other value is a configure-time `FATAL_ERROR` naming the option and the allowed values.
  - `MORPHEUS_WARNINGS_AS_ERRORS`, default `PROJECT_IS_TOP_LEVEL`: `/WX` or `-Werror` on Morpheus's own targets.
  - `MORPHEUS_ASAN`, default `OFF`: unchanged.
  - `MORPHEUS_VULKAN` is removed everywhere.
- **GPU switches:** `GGML_VULKAN` ON only for `Vulkan`, `GGML_CUDA` ON only for `CUDA`, both OFF for `CPU`, set with `FORCE`.
- **llama.cpp:** `https://github.com/ggml-org/llama.cpp.git` at `960d628f46d17ea9c10aa944b061b2341afeb3b5` (b9804); the TODO comment goes.
- **Vendored builds:** only `morpheus_core`, `morpheus_store`, `morpheus_inference`, `morpheus_context`, `morpheus_turn`, plus nlohmann/json and SQLite; no llama.cpp, SDL3, ImGui or Catch2. `CMAKE_RUNTIME_OUTPUT_DIRECTORY`, `CMAKE_LIBRARY_OUTPUT_DIRECTORY`, `CMAKE_ARCHIVE_OUTPUT_DIRECTORY` and `VS_STARTUP_PROJECT` only at top level.
- **Targets:** `sqlite3` becomes `morpheus_sqlite3`. nlohmann/json is fetched only `if(NOT TARGET nlohmann_json::nlohmann_json)`. Aliases: `Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::turn`, `Morpheus::llama` (when built), `Morpheus::app_model` and `Morpheus::app_ui` (when built).
- **Presets:** `linux-tests` (Ninja, `CMAKE_BUILD_TYPE` `Debug`, `CMAKE_C_COMPILER` `clang`, `CMAKE_CXX_COMPILER` `clang++`, `MORPHEUS_BUILD_APP` OFF, `MORPHEUS_BUILD_LLAMA` OFF, `MORPHEUS_BUILD_TESTS` ON, binary dir `build/linux-tests`) and `linux-release` (Ninja, `Release`, clang, app/llama/tests ON, `MORPHEUS_GPU_BACKEND` `CUDA`, `CMAKE_CUDA_ARCHITECTURES` `native`, binary dir `build/linux-release`), each with matching build and test presets, conditioned on `${hostSystemName}` equal to `Linux`. `dev`, `tests`, `asan`, `dev-asan` conditioned on `Windows`.
- **Consumer check:** `cmake/consumer-check/` with `CMakeLists.txt` and `main.cpp`; `FetchContent_Declare(morpheus SOURCE_DIR <path>)` then `FetchContent_MakeAvailable(morpheus)`; the path from the cache variable `MORPHEUS_SOURCE_DIR`, default the repository root two levels up; links `Morpheus::turn`.
- **No CI.** `MORPHEUS_BUILD_SERVER` is not added (sub-project D). llama.cpp is not updated (sub-project C).
- **Coding rules (`CLAUDE.md`):** fail fast and loud, no fallbacks; comments let a file read top to bottom; constants ALL_CAPS; ASCII only in source comments and docs; no legacy paths kept for old behavior.
- **Shell:** commands run from `D:\MorpheusEngine` in Git Bash. `$(pwd -W)` gives the Windows form of the repo path (`D:/MorpheusEngine`) for `-D` arguments; never pass `$PWD` (`/d/...`) to CMake. Long builds may be run in the background.
- **Baseline today:** `ctest --preset tests` passes 315 tests; `build/dev` is configured; `cmake --build build/dev --config RelWithDebInfo --target morpheus` then `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120` exits 0. `ninja` is not on this machine's PATH.
- **Linux** checks cannot run on this machine. They are the owner's manual acceptance in Task 4; no step claims them.
- **Never stage** `settings.json`, `models/`, `imgui.ini`, `.superpowers/`, `docs/references/` or anything under `build/`. Stage the files each commit step names, by explicit path.
- **Commits** are made on `master`. Every commit message ends with a `Co-Authored-By:` line naming the implementing model. The steps show `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; an implementer running as Opus writes `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` instead.


## Decisions the plan made (binding)

**Choices this plan makes where the spec is silent, ambiguous or contradicts the code (the owner may overrule any of them):**
- **`dev` preset (spec contradiction).** The spec says the Windows presets are "unchanged except that `dev-asan` sets `MORPHEUS_GPU_BACKEND` to `CPU`", but also that "every preset ... that set [`MORPHEUS_VULKAN`] uses `MORPHEUS_GPU_BACKEND`", and `dev` sets `"MORPHEUS_VULKAN": "ON"`. `dev` gets `"MORPHEUS_GPU_BACKEND": "Vulkan"` in its place (the same build; `tests` and `asan` inherit it, unused there).
- **The backend check runs on every configure**, also with `MORPHEUS_BUILD_LLAMA` off, so a misspelt value never passes silently. It is case-sensitive: `vulkan` fails like `Metal`. The message is `MORPHEUS_GPU_BACKEND is '<value>'; it must be one of: Vulkan, CUDA, CPU.`
- **Stale cache entry.** The existing `build/dev`, `build/tests` and `build/asan` caches hold `MORPHEUS_VULKAN`. Reconfiguring passes `-U MORPHEUS_VULKAN` once to drop it, so nothing reads as if the old option still worked.
- **llama.cpp re-clone.** Changing `GIT_REPOSITORY` makes FetchContent clone llama.cpp again into `build/dev/_deps/llama-src` and rebuild it (Vulkan shaders included) on the next `dev` build. Same commit, same code; it only costs time.
- **Consumer check extras.** Besides the six targets the spec names (`morpheus`, `morpheus_tests`, `morpheus_llama`, `imgui_core`, `SDL3::SDL3`, `Catch2::Catch2`), it also refuses `morpheus_app_model`, `morpheus_app_ui`, `morpheus_probe`, `morpheus_play` and `llama`: the spec's "Vendored builds" says only the five engine libraries are built. Its project is `LANGUAGES CXX` only (a C++-only host; Morpheus enables C itself for SQLite). "The host's ordinary warnings" is `/W4` (MSVC) or `-Wall -Wextra` on `consumer_check`, without `/WX` / `-Werror`.
- **Consumer check program.** It calls `core::DiceRule::Parse("1d20+stat")`, `core::CheckSeed` and `core::RollCheck` (core), and `turn::ParseReviewOutput` (turn): pure functions, no files, no backend. It prints `consumer_check: ok (...)` and returns 0, or prints what was unexpected and returns 1.
- **Clash proof.** The spec's consumer check does not exercise the nlohmann guard or the SQLite rename (with no host copy, nothing can clash). Task 2 proves both with a throwaway host in `build/clash-check/` (gitignored, deleted after) that brings its own nlohmann/json under another FetchContent name and its own `sqlite3` target. It is not committed.
- **Windows conditions with the Linux presets.** The Windows-only `condition` on the four Windows presets is added in Task 3, together with the Linux presets, since the two only make sense together. Every build and test preset gets the same `condition` as its configure preset, so neither OS lists the other's build or test presets.
- **The Windows-only test** is named in the spec as "SaveSettingsEdit reports a file it cannot replace"; its full name is "SaveSettingsEdit reports a file it cannot replace and leaves no check file". After the guard, `ctest --preset tests` still runs 315 tests on Windows; `linux-tests` runs 314.
- **Internal links keep the internal names** (`morpheus_turn`, ...). The aliases are for hosts.
- **Linux packages in the README.** `linux-release` builds llama.cpp with CUDA, so the Vulkan packages (`libvulkan-dev`, `glslc`, `spirv-headers`) and the container/Vulkan paragraph move to a note on building with `-DMORPHEUS_GPU_BACKEND=Vulkan`. `nvcc` compiles host code with the system's `g++` unless told otherwise, so `g++` is listed for `linux-release`. `linux-tests` needs only `clang`, `cmake` and `ninja-build`.
- **Documentation beyond the spec's list**, to keep it true: the architecture spec's "It is built with Vulkan." (inference section) names the backend choice; the README's performance note says its measurement used a Vulkan build; "(`dev` preset only)" in the CLAUDE.md and README layout lists becomes "(`dev` and `linux-release` presets only)".
- **Not addressed (spec silent):** Morpheus's FetchContent names `sqlite` and `llama` could still meet a host's own FetchContent of the same name (the first declaration wins). `.superpowers/sdd/global-constraints.md` still lists `/WX` as unconditional; it is the controller's notes and is never staged here.
- **The Linux consumer check** uses the spec's command, which builds with the system's default `c++` (g++ on Ubuntu 24.04), a compiler Morpheus has not been built with. Without `-Werror` its warnings do not fail the build; the owner's acceptance list says how to tell a compiler problem from a vendoring problem.

