# Consumable, Linux-buildable Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Morpheus can be pulled into another CMake project with FetchContent and builds only its engine libraries there without target clashes or warnings-as-errors; it builds and tests natively on Linux from presets; and llama.cpp's GPU backend is a choice (Vulkan, CUDA or CPU). Building Morpheus on its own on Windows stays exactly as today.

**Architecture:**
- **Root `CMakeLists.txt`:** the build options default to `PROJECT_IS_TOP_LEVEL`. `MORPHEUS_GPU_BACKEND` (a validated cache STRING) replaces `MORPHEUS_VULKAN` and sets llama.cpp's `GGML_VULKAN` / `GGML_CUDA` with `FORCE`. `morpheus_target_defaults` adds `/WX` / `-Werror` only with `MORPHEUS_WARNINGS_AS_ERRORS`. Output directories and the Visual Studio startup project are set only at top level. llama.cpp moves to `ggml-org` at the same commit.
- **No clashes:** SQLite's target becomes `morpheus_sqlite3`; nlohmann/json is fetched only when no `nlohmann_json::nlohmann_json` exists; every engine library gets a `Morpheus::` alias next to its `add_library`.
- **Checks:** `cmake/consumer-check/` (committed) includes Morpheus as a host would and fails to configure if anything beyond the engine libraries exists. Two throwaway projects under the gitignored `build/` prove the option changes (`build/options-check`) and the clash fixes (`build/clash-check`) before and after.
- **Presets:** `linux-tests` and `linux-release` (CUDA), shown only on Linux; the four Windows presets gain a Windows-only condition. The one Windows-only Catch2 case is compiled only on Windows.
- **Spec:** `docs/superpowers/specs/2026-11-13-consumable-build-design.md` (the single source of requirements).

**Tech Stack:** CMake >= 3.25 (this machine has 4.4.0-rc2), CMake presets version 6, FetchContent, MSVC through `Visual Studio 17 2022` on Windows; clang, Ninja and the CUDA toolkit on the owner's Ubuntu 24.04 desktop. No new dependencies.

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

## File Structure

```
CMakeLists.txt                       modify: options, GPU backend, warnings, output dirs, startup project,
                                     llama.cpp URL (Task 1); nlohmann guard, morpheus_sqlite3 (Task 2)
CMakePresets.json                    modify: MORPHEUS_GPU_BACKEND (Task 1); Windows conditions, linux-tests,
                                     linux-release (Task 3)
src/core/CMakeLists.txt              modify: Morpheus::core (Task 2)
src/store/CMakeLists.txt             modify: morpheus_sqlite3, Morpheus::store (Task 2)
src/inference/CMakeLists.txt         modify: Morpheus::inference (Task 2)
src/context/CMakeLists.txt           modify: Morpheus::context (Task 2)
src/turn/CMakeLists.txt              modify: Morpheus::turn (Task 2)
src/llama/CMakeLists.txt             modify: Morpheus::llama (Task 2)
src/app/CMakeLists.txt               modify: Morpheus::app_model, Morpheus::app_ui (Task 2)
cmake/consumer-check/CMakeLists.txt  create: the host project and its target checks (Task 2)
cmake/consumer-check/main.cpp        create: calls into core and turn (Task 2)
tests/app/SettingsEditTests.cpp      modify: the Windows-only case under #ifdef _WIN32 (Task 3)
README.md                            modify: Windows backend note, Build (Linux), Using Morpheus from another
                                     project, layout (Task 4)
CLAUDE.md                            modify: build section, layout (Task 4)
docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md   modify: Build paragraph, backend line (Task 4)

build/options-check/                 throwaway (gitignored): option checks, Task 1
build/clash-check/                   throwaway (gitignored): clash checks, Task 2
build/consumer-check/                gitignored: the consumer check's build tree, Tasks 2 and 4
```

---

### Task 1: Options, GPU backend choice, warnings, output directories, llama.cpp URL

The root `CMakeLists.txt` gets the spec's options with their top-level defaults, the validated `MORPHEUS_GPU_BACKEND`, conditional warnings-as-errors and top-level-only output directories and startup project; llama.cpp moves to its new URL; the presets stop setting `MORPHEUS_VULKAN`. A top-level Windows build is unchanged.

**Files:**
- Modify: `CMakeLists.txt` (lines 11-25 options and output dirs, 40-50 `morpheus_target_defaults`, 77-90 llama.cpp, 185-188 startup project)
- Modify: `CMakePresets.json` (`dev` and `dev-asan` cache variables)
- Test: `build/options-check/` (throwaway configure), `build/tests`, `build/dev`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - Cache options `MORPHEUS_BUILD_APP`, `MORPHEUS_BUILD_TESTS`, `MORPHEUS_BUILD_LLAMA`, `MORPHEUS_WARNINGS_AS_ERRORS` (BOOL, default `${PROJECT_IS_TOP_LEVEL}`), `MORPHEUS_ASAN` (BOOL, OFF), `MORPHEUS_GPU_BACKEND` (STRING `Vulkan`|`CUDA`|`CPU`, default `Vulkan`).
  - `morpheus_target_defaults(target)`: same name and signature; `/WX` / `-Werror` only when `MORPHEUS_WARNINGS_AS_ERRORS`.
  - Task 2's consumer check relies on the vendored defaults (everything OFF) and on Morpheus not setting output directories when vendored.

- [ ] **Step 1: Run the option checks to see them fail**

These configure a throwaway tree with only the engine libraries plus llama.cpp, reusing the sources already downloaded into `build/tests` and `build/dev`, so nothing is fetched from the network.

Run:

```bash
cmake -S . -B build/options-check -G "Visual Studio 17 2022" -A x64 \
  -DMORPHEUS_BUILD_APP=OFF -DMORPHEUS_BUILD_TESTS=OFF -DMORPHEUS_BUILD_LLAMA=OFF \
  -DMORPHEUS_GPU_BACKEND=Metal \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN="$(pwd -W)/build/tests/_deps/nlohmann-src" \
  -DFETCHCONTENT_SOURCE_DIR_SQLITE="$(pwd -W)/build/tests/_deps/sqlite-src" \
  -DFETCHCONTENT_SOURCE_DIR_LLAMA="$(pwd -W)/build/dev/_deps/llama-src"; echo "exit $?"
```

Expected (FAIL: an unknown backend is accepted): the configure succeeds, prints a `Manually-specified variables were not used by the project:` warning listing `MORPHEUS_GPU_BACKEND` (and `FETCHCONTENT_SOURCE_DIR_LLAMA`, unused while llama.cpp is off), and ends with `exit 0`. The `-D` values are cached, so the next commands reuse the three source folders.

Run:

```bash
cmake -S . -B build/options-check -DMORPHEUS_BUILD_LLAMA=ON -DMORPHEUS_GPU_BACKEND=CPU -DMORPHEUS_WARNINGS_AS_ERRORS=OFF; echo "exit $?"
grep -E '^(GGML_VULKAN|GGML_CUDA|MORPHEUS_GPU_BACKEND|MORPHEUS_WARNINGS_AS_ERRORS):' build/options-check/CMakeCache.txt
grep -c 'TreatWarningAsError>true' build/options-check/src/core/morpheus_core.vcxproj
```

Expected (FAIL: `CPU` still builds Vulkan, and warnings stay errors):

```
exit 0
GGML_CUDA:BOOL=OFF
GGML_VULKAN:BOOL=ON
MORPHEUS_GPU_BACKEND:UNINITIALIZED=CPU
MORPHEUS_WARNINGS_AS_ERRORS:UNINITIALIZED=OFF
4
```

(`4`: one `<TreatWarningAsError>true</TreatWarningAsError>` per configuration, Debug, Release, MinSizeRel and RelWithDebInfo.)

Run: `grep -n 'llama.cpp.git' CMakeLists.txt`
Expected (FAIL): `84:        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git     # repo TODO: ...`

- [ ] **Step 2: The output directories at top level only**

In `CMakeLists.txt`, replace

```cmake
# Put all binaries in one place - handy on Windows for DLL discovery
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For executables: .exe, .dll
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For shared libs: .so on linux
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # For static libs: .a on linux, .lib on win
```

with

```cmake
# Put all binaries in one place - handy on Windows for DLL discovery. Only when Morpheus is the top-level project:
# a host project that includes Morpheus decides where binaries go.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For executables: .exe, .dll
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For shared libs: .so on linux
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # For static libs: .a on linux, .lib on win
endif()
```

- [ ] **Step 3: The options and the GPU backend choice**

In `CMakeLists.txt`, replace

```cmake
# ---- Options ------------------------------------------------------------
option(MORPHEUS_BUILD_APP "Build the morpheus app (needs MORPHEUS_BUILD_LLAMA; fetches SDL3)" ON)
option(MORPHEUS_BUILD_TESTS "Build morpheus_tests (fetches Catch2)" ON)
option(MORPHEUS_ASAN "Instrument every target with AddressSanitizer (MSVC only)" OFF)
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend (fetches and builds llama.cpp)" ON)
option(MORPHEUS_VULKAN "Build llama.cpp with the Vulkan GPU backend (needs the Vulkan SDK)" ON)
```

with

```cmake
# ---- Options ------------------------------------------------------------
# The app, the tests, llama.cpp and warnings as errors default to on only when Morpheus is the top-level project. A
# host project that includes Morpheus (FetchContent, add_subdirectory) gets the engine libraries alone, and a warning
# from the host's compiler in Morpheus's code does not fail the host's build.
option(MORPHEUS_BUILD_APP "Build the morpheus app (needs MORPHEUS_BUILD_LLAMA; fetches SDL3)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_BUILD_TESTS "Build morpheus_tests (fetches Catch2, and ImGui's core for the headless UI tests)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend, morpheus_probe and morpheus_play (fetches and builds llama.cpp)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_WARNINGS_AS_ERRORS "Treat warnings as errors on MorpheusEngine's own targets (/WX or -Werror)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_ASAN "Instrument every target with AddressSanitizer (MSVC only)" OFF)

# llama.cpp's GPU backend. Checked on every configure, with or without MORPHEUS_BUILD_LLAMA, so that a misspelt value
# never passes silently.
set(MORPHEUS_GPU_BACKENDS Vulkan CUDA CPU)
set(MORPHEUS_GPU_BACKEND "Vulkan" CACHE STRING
    "llama.cpp's GPU backend: Vulkan (needs the Vulkan SDK), CUDA (needs the CUDA toolkit) or CPU")
set_property(CACHE MORPHEUS_GPU_BACKEND PROPERTY STRINGS ${MORPHEUS_GPU_BACKENDS})
if(NOT MORPHEUS_GPU_BACKEND IN_LIST MORPHEUS_GPU_BACKENDS)
    list(JOIN MORPHEUS_GPU_BACKENDS ", " MORPHEUS_GPU_BACKEND_NAMES)
    message(FATAL_ERROR "MORPHEUS_GPU_BACKEND is '${MORPHEUS_GPU_BACKEND}'; it must be one of: ${MORPHEUS_GPU_BACKEND_NAMES}.")
endif()
```

The `if(MORPHEUS_BUILD_APP AND NOT MORPHEUS_BUILD_LLAMA)` `FATAL_ERROR` right after stays as it is.

- [ ] **Step 4: Warnings as errors only with the option**

In `CMakeLists.txt`, replace

```cmake
# Strict settings for MorpheusEngine's own targets. Never applied to fetched dependencies.
function(morpheus_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        # /W4 warning level, /WX warnings are errors, /permissive- standard compliance,
        # /Zc:preprocessor standard preprocessor, /utf-8 source and execution character sets.
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:preprocessor /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()
endfunction()
```

with

```cmake
# Strict settings for MorpheusEngine's own targets. Never applied to fetched dependencies. The warnings are always on;
# they fail the build only with MORPHEUS_WARNINGS_AS_ERRORS (on by default at top level, off when vendored).
function(morpheus_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        # /W4 warning level, /permissive- standard compliance, /Zc:preprocessor standard preprocessor,
        # /utf-8 source and execution character sets; /WX makes warnings errors.
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8)
        if(MORPHEUS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(MORPHEUS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
```

- [ ] **Step 5: llama.cpp's backend switches and its new URL**

In `CMakeLists.txt`, replace

```cmake
if(MORPHEUS_BUILD_LLAMA)
    # GPU backend for llama.cpp. FORCE so that toggling MORPHEUS_VULKAN reconfigures the fetched llama.cpp.
    set(GGML_VULKAN ${MORPHEUS_VULKAN} CACHE BOOL "" FORCE)

    # llama
    FetchContent_Declare(
        llama                                                         # prefix for CMake variables
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git     # repo TODO: redirect to the new repo: ggml-org/llama.cpp , probably update the targeted commit?
        GIT_TAG        960d628f46d17ea9c10aa944b061b2341afeb3b5       # commit SHA to get, version b9804
```

with

```cmake
if(MORPHEUS_BUILD_LLAMA)
    # llama.cpp's own backend switches follow MORPHEUS_GPU_BACKEND: GGML_VULKAN only for Vulkan, GGML_CUDA only for
    # CUDA, neither for CPU. FORCE so that changing the choice reconfigures the fetched llama.cpp.
    set(MORPHEUS_GGML_VULKAN OFF)
    set(MORPHEUS_GGML_CUDA OFF)
    if(MORPHEUS_GPU_BACKEND STREQUAL "Vulkan")
        set(MORPHEUS_GGML_VULKAN ON)
    elseif(MORPHEUS_GPU_BACKEND STREQUAL "CUDA")
        set(MORPHEUS_GGML_CUDA ON)
    endif()
    set(GGML_VULKAN ${MORPHEUS_GGML_VULKAN} CACHE BOOL "" FORCE)
    set(GGML_CUDA ${MORPHEUS_GGML_CUDA} CACHE BOOL "" FORCE)

    # llama
    FetchContent_Declare(
        llama                                                         # prefix for CMake variables
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git      # repo
        GIT_TAG        960d628f46d17ea9c10aa944b061b2341afeb3b5       # commit SHA to get, version b9804
```

- [ ] **Step 6: The Visual Studio startup project at top level only**

In `CMakeLists.txt`, replace

```cmake
if(MORPHEUS_BUILD_APP)
    # Visual Studio opens the solution with the app as the startup project.
    set_property(DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT morpheus)
endif()
```

with

```cmake
if(MORPHEUS_BUILD_APP AND PROJECT_IS_TOP_LEVEL)
    # Visual Studio opens the solution with the app as the startup project. A host's solution keeps its own.
    set_property(DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT morpheus)
endif()
```

- [ ] **Step 7: The presets use MORPHEUS_GPU_BACKEND**

In `CMakePresets.json`, in the `dev` configure preset, replace

```json
        "MORPHEUS_BUILD_LLAMA": "ON",
        "MORPHEUS_VULKAN": "ON"
```

with

```json
        "MORPHEUS_BUILD_LLAMA": "ON",
        "MORPHEUS_GPU_BACKEND": "Vulkan"
```

In the `dev-asan` configure preset, replace

```json
        "MORPHEUS_VULKAN": "OFF",
```

with

```json
        "MORPHEUS_GPU_BACKEND": "CPU",
```

Run: `grep -rn MORPHEUS_VULKAN CMakeLists.txt CMakePresets.json src tools tests`
Expected: no output.

- [ ] **Step 8: Run the option checks to see them pass**

Run:

```bash
cmake -S . -B build/options-check -DMORPHEUS_BUILD_LLAMA=OFF -DMORPHEUS_GPU_BACKEND=Metal; echo "exit $?"
```

Expected (PASS):

```
CMake Error at CMakeLists.txt:<line> (message):
  MORPHEUS_GPU_BACKEND is 'Metal'; it must be one of: Vulkan, CUDA, CPU.


-- Configuring incomplete, errors occurred!
exit 1
```

Run:

```bash
cmake -S . -B build/options-check -DMORPHEUS_BUILD_LLAMA=ON -DMORPHEUS_GPU_BACKEND=CPU -DMORPHEUS_WARNINGS_AS_ERRORS=OFF; echo "exit $?"
grep -E '^(GGML_VULKAN|GGML_CUDA|MORPHEUS_GPU_BACKEND|MORPHEUS_WARNINGS_AS_ERRORS):' build/options-check/CMakeCache.txt
grep -c 'TreatWarningAsError>true' build/options-check/src/core/morpheus_core.vcxproj
grep -c 'WarningLevel>Level4' build/options-check/src/core/morpheus_core.vcxproj
```

Expected (PASS: CPU turns both GPU switches off; warnings stay on but are not errors):

```
exit 0
GGML_CUDA:BOOL=OFF
GGML_VULKAN:BOOL=OFF
MORPHEUS_GPU_BACKEND:STRING=CPU
MORPHEUS_WARNINGS_AS_ERRORS:BOOL=OFF
0
4
```

Run:

```bash
cmake -S . -B build/options-check -DMORPHEUS_GPU_BACKEND=Vulkan; echo "exit $?"
grep -E '^(GGML_VULKAN|GGML_CUDA):' build/options-check/CMakeCache.txt
```

Expected (PASS: changing the choice back reconfigures llama.cpp): `exit 0`, then `GGML_CUDA:BOOL=OFF` and `GGML_VULKAN:BOOL=ON`.

Run: `grep -n 'llama.cpp.git' CMakeLists.txt`
Expected: one line, `GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git      # repo`, with no TODO.

Run: `rm -rf build/options-check`

- [ ] **Step 9: The tests preset is unchanged**

Run:

```bash
cmake --preset tests -U MORPHEUS_VULKAN && cmake --build --preset tests && ctest --preset tests
grep -c 'TreatWarningAsError>true' build/tests/src/core/morpheus_core.vcxproj
grep -c MORPHEUS_VULKAN build/tests/CMakeCache.txt
```

Expected: the build has no warnings, `100% tests passed, 0 tests failed out of 315`, then `4` (top level still treats warnings as errors), then `0` (the old option is gone from the cache).

- [ ] **Step 10: The dev preset is unchanged and the app starts**

Reconfiguring `dev` clones llama.cpp again from its new URL (same commit) and the next build rebuilds llama.cpp, Vulkan shaders included: expect several minutes; run the build in the background if needed.

Run:

```bash
cmake --preset dev -U MORPHEUS_VULKAN
grep -E '^(GGML_VULKAN|GGML_CUDA|MORPHEUS_GPU_BACKEND):' build/dev/CMakeCache.txt
grep -c MORPHEUS_VULKAN build/dev/CMakeCache.txt
```

Expected: the configure ends with `-- Generating done` and `-- Build files have been written to: D:/MorpheusEngine/build/dev`; then

```
GGML_CUDA:BOOL=OFF
GGML_VULKAN:BOOL=ON
MORPHEUS_GPU_BACKEND:STRING=Vulkan
0
```

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus`
Expected: builds with no warnings in Morpheus's own targets.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120; echo "exit $?"`
Expected: the window opens and closes; `exit 0`.

Run: `cmake --preset asan -U MORPHEUS_VULKAN`
Expected: configures (`-- Generating done`); the asan build and run are in Task 4.

- [ ] **Step 11: Commit**

```bash
git add CMakeLists.txt CMakePresets.json
git commit -m "Choose llama.cpp's GPU backend, default the build options to the top-level project" -m "MORPHEUS_GPU_BACKEND (Vulkan, CUDA or CPU) replaces MORPHEUS_VULKAN. The app, tests, llama.cpp and warnings as errors default to PROJECT_IS_TOP_LEVEL; output directories and the startup project are set only at top level. llama.cpp is fetched from ggml-org at the same commit." -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 2: No target clashes, namespaced aliases, the consumer check

SQLite's target is renamed, nlohmann/json is fetched only when the host has none, every engine library gets its `Morpheus::` alias, and `cmake/consumer-check/` proves that a host including Morpheus gets only the engine libraries.

**Files:**
- Create: `cmake/consumer-check/CMakeLists.txt`, `cmake/consumer-check/main.cpp`
- Modify: `CMakeLists.txt` (lines 55-75, nlohmann and SQLite), `src/core/CMakeLists.txt`, `src/store/CMakeLists.txt`, `src/inference/CMakeLists.txt`, `src/context/CMakeLists.txt`, `src/turn/CMakeLists.txt`, `src/llama/CMakeLists.txt`, `src/app/CMakeLists.txt`
- Test: `build/consumer-check/` (the consumer check's build), `build/clash-check/` (throwaway)

**Interfaces:**
- Consumes (Task 1): with Morpheus not top level, `MORPHEUS_BUILD_APP`, `MORPHEUS_BUILD_TESTS`, `MORPHEUS_BUILD_LLAMA` and `MORPHEUS_WARNINGS_AS_ERRORS` default OFF, and no output directories are set.
- Produces:
  - Targets `morpheus_sqlite3` (static, the SQLite amalgamation) and the aliases `Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::turn`, `Morpheus::llama` (with `MORPHEUS_BUILD_LLAMA`), `Morpheus::app_model`, `Morpheus::app_ui` (with the app or the tests).
  - `cmake/consumer-check/`: cache variable `MORPHEUS_SOURCE_DIR` (PATH, default the repo root), executable `consumer_check`. Task 4's README documents it.

- [ ] **Step 1: Write the consumer check**

Create `cmake/consumer-check/CMakeLists.txt`:

```cmake
# Consumer check: includes Morpheus the way a host project would (FetchContent), but from a local checkout, and checks
# that the vendored build defines only the engine libraries. See the README, "Using Morpheus from another project".
cmake_minimum_required(VERSION 3.25)

# A C++-only host: Morpheus enables C itself for the SQLite amalgamation.
project(MorpheusConsumerCheck LANGUAGES CXX)

# The checkout to include: by default the repository this folder is in, two levels up.
cmake_path(SET MORPHEUS_DEFAULT_SOURCE_DIR NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(MORPHEUS_SOURCE_DIR "${MORPHEUS_DEFAULT_SOURCE_DIR}" CACHE PATH "The MorpheusEngine checkout to include")

include(FetchContent)
FetchContent_Declare(morpheus SOURCE_DIR "${MORPHEUS_SOURCE_DIR}")
FetchContent_MakeAvailable(morpheus)

# With Morpheus's vendored defaults nothing beyond the five engine libraries exists: no app, tests or tools, no
# app-model or app-ui library, and none of llama.cpp, SDL3, ImGui or Catch2.
set(MORPHEUS_UNWANTED_TARGETS
    morpheus morpheus_tests morpheus_llama imgui_core SDL3::SDL3 Catch2::Catch2
    morpheus_app_model morpheus_app_ui morpheus_probe morpheus_play llama
)
foreach(unwanted IN LISTS MORPHEUS_UNWANTED_TARGETS)
    if(TARGET ${unwanted})
        message(FATAL_ERROR "The vendored Morpheus build defines the target '${unwanted}'; with its default options it must build only the engine libraries.")
    endif()
endforeach()
if(NOT TARGET Morpheus::turn)
    message(FATAL_ERROR "The vendored Morpheus build does not define Morpheus::turn.")
endif()

# The host's own program, with ordinary warnings and no warnings as errors.
add_executable(consumer_check main.cpp)
target_link_libraries(consumer_check PRIVATE Morpheus::turn)
if(MSVC)
    target_compile_options(consumer_check PRIVATE /W4)
else()
    target_compile_options(consumer_check PRIVATE -Wall -Wextra)
endif()
```

Create `cmake/consumer-check/main.cpp`:

```cpp
// Consumer check: a host program that links Morpheus::turn includes headers from core and turn and calls into both.
// It needs no files and no model. It exits with 0 when both calls give the expected results.
#include <cstdio>

#include "core/Dice.h"
#include "turn/Review.h"

int main()
{
    using namespace morpheus;

    // core: the usual world roll, one d20 plus a stat of 3 against 10, with a fixed seed. The die lands in 1..20 and
    // the total is the die plus the stat.
    const int statValue = 3;
    const core::DiceRule rule = core::DiceRule::Parse("1d20+stat");
    const core::CheckResult check = core::RollCheck(rule, statValue, 10, core::CheckSeed(42, 1, 0));
    const bool diceOk = check.rolls.size() == 1 && check.rolls[0] >= 1 && check.rolls[0] <= 20 &&
                        check.total == check.rolls[0] + statValue;

    // turn: a review that finds the narration consistent, with no problems, reads back as exactly that.
    const turn::Review review = turn::ParseReviewOutput(R"({"consistent":true,"problems":[]})", true, false);
    const bool reviewOk = review.consistent == true && review.problems.empty() && review.error.empty();

    if (!diceOk || !reviewOk) {
        std::fprintf(stderr, "consumer_check: unexpected result (dice %s, review %s)\n", diceOk ? "ok" : "wrong",
                     reviewOk ? "ok" : "wrong");
        return 1;
    }
    std::printf("consumer_check: ok (rolled %d, total %lld)\n", check.rolls[0], check.total);
    return 0;
}
```

- [ ] **Step 2: Run the consumer check to see it fail**

Run: `ninja --version`
Expected on this machine: `ninja: command not found`, so the Visual Studio generator is used below. (Where Ninja is present, the spec's command `cmake -S cmake/consumer-check -B build/consumer-check -G Ninja && cmake --build build/consumer-check && ./build/consumer-check/consumer_check` is the same check.)

Run:

```bash
cmake -S cmake/consumer-check -B build/consumer-check -G "Visual Studio 17 2022" -A x64; echo "exit $?"
```

Expected (FAIL: no alias yet; the first configure clones nlohmann/json and downloads SQLite):

```
CMake Error at CMakeLists.txt:<line> (message):
  The vendored Morpheus build does not define Morpheus::turn.


-- Configuring incomplete, errors occurred!
exit 1
```

No `defines the target` error appears: Task 1's defaults already keep the app, tests and llama.cpp out.

- [ ] **Step 3: Write the clash check and see it fail**

A throwaway host (under the gitignored `build/`, never committed) that brings its own nlohmann/json under another FetchContent name and its own target named `sqlite3`, then includes Morpheus.

Run:

```bash
mkdir -p build/clash-check/src
cat > build/clash-check/src/CMakeLists.txt <<'EOF'
# Throwaway: a host with its own nlohmann/json (declared as "json") and its own target named sqlite3 includes Morpheus.
cmake_minimum_required(VERSION 3.25)
project(MorpheusClashCheck LANGUAGES CXX)
include(FetchContent)
FetchContent_Declare(json SOURCE_DIR "${HOST_JSON_SOURCE_DIR}")
FetchContent_MakeAvailable(json)
add_library(sqlite3 INTERFACE)
FetchContent_Declare(morpheus SOURCE_DIR "${MORPHEUS_SOURCE_DIR}")
FetchContent_MakeAvailable(morpheus)
EOF
cmake -S build/clash-check/src -B build/clash-check/out -G "Visual Studio 17 2022" -A x64 \
  -DHOST_JSON_SOURCE_DIR="$(pwd -W)/build/tests/_deps/nlohmann-src" -DMORPHEUS_SOURCE_DIR="$(pwd -W)"; echo "exit $?"
```

Expected (FAIL): Morpheus clones its own nlohmann/json and the configure reports

```
CMake Error at D:/MorpheusEngine/build/clash-check/out/_deps/nlohmann-src/CMakeLists.txt:<line> (add_library):
  add_library cannot create target "nlohmann_json" because another target with
  the same name already exists.
...
CMake Error at D:/MorpheusEngine/CMakeLists.txt:<line> (add_library):
  add_library cannot create target "sqlite3" because another target with the
  same name already exists.
...
-- Configuring incomplete, errors occurred!
exit 1
```

(an `add_library cannot create ALIAS target "nlohmann_json::nlohmann_json"` error may appear between them).

Run: `rm -rf build/clash-check/out`

- [ ] **Step 4: Guard nlohmann/json and rename SQLite's target**

In `CMakeLists.txt`, replace

```cmake
# nlohmann (engine libraries and app)
FetchContent_Declare(
    nlohmann                                                      # prefix for CMake variables
    GIT_REPOSITORY https://github.com/nlohmann/json.git           # repo
    GIT_TAG        55f93686c01528224f448c19128836e7df245f72       # commit SHA to get, 3.12.0
    GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
    SYSTEM                                                        # Treat its headers as external (no /W4 /WX)
)
FetchContent_MakeAvailable(nlohmann)
```

with

```cmake
# nlohmann (engine libraries and app). A host project that already provides it (its own FetchContent or find_package)
# keeps its copy, and Morpheus links the same target.
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann                                                      # prefix for CMake variables
        GIT_REPOSITORY https://github.com/nlohmann/json.git           # repo
        GIT_TAG        55f93686c01528224f448c19128836e7df245f72       # commit SHA to get, 3.12.0
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM                                                        # Treat its headers as external (no /W4 /WX)
    )
    FetchContent_MakeAvailable(nlohmann)
endif()
```

and replace

```cmake
# SQLite amalgamation (store). The archive has no CMakeLists.txt, so the library target is defined here.
FetchContent_Declare(
    sqlite
    URL      https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip   # SQLite 3.50.4
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite)
add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
```

with

```cmake
# SQLite amalgamation (store). The archive has no CMakeLists.txt, so the library target is defined here, under a name
# of Morpheus's own so that it never meets a host project's sqlite3 target.
FetchContent_Declare(
    sqlite
    URL      https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip   # SQLite 3.50.4
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite)
add_library(morpheus_sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(morpheus_sqlite3 SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(morpheus_sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
```

- [ ] **Step 5: The aliases, and store links morpheus_sqlite3**

Replace the whole of `src/core/CMakeLists.txt` with:

```cmake
add_library(morpheus_core STATIC
    ContentHash.cpp
    Csv.cpp
    JsonInt.cpp
    MemoryMode.cpp
    Settings.cpp
    TurnRecord.cpp
    StateSchema.cpp
    Changes.cpp
    Dice.cpp
    Text.cpp
    World.cpp
)
add_library(Morpheus::core ALIAS morpheus_core) # The name host projects link.
target_include_directories(morpheus_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_core PUBLIC nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_core)
```

Replace the whole of `src/store/CMakeLists.txt` with:

```cmake
add_library(morpheus_store STATIC
    Sqlite.cpp
    RunStore.cpp
)
add_library(Morpheus::store ALIAS morpheus_store) # The name host projects link.
target_include_directories(morpheus_store PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_store
    PUBLIC morpheus_core
    PRIVATE morpheus_sqlite3
)
morpheus_target_defaults(morpheus_store)
```

Replace the whole of `src/inference/CMakeLists.txt` with:

```cmake
add_library(morpheus_inference STATIC
    Llm.cpp
)
add_library(Morpheus::inference ALIAS morpheus_inference) # The name host projects link.
target_include_directories(morpheus_inference PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_inference PUBLIC morpheus_core)
morpheus_target_defaults(morpheus_inference)
```

Replace the whole of `src/context/CMakeLists.txt` with:

```cmake
add_library(morpheus_context STATIC
    PromptBuilder.cpp
    StateView.cpp
    Transcript.cpp
    WorldReference.cpp
)
add_library(Morpheus::context ALIAS morpheus_context) # The name host projects link.
target_include_directories(morpheus_context PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_context PUBLIC morpheus_core morpheus_inference)
morpheus_target_defaults(morpheus_context)
```

Replace the whole of `src/turn/CMakeLists.txt` with:

```cmake
add_library(morpheus_turn STATIC
    Folding.cpp
    Replay.cpp
    Resolve.cpp
    ResolveGrammar.cpp
    Retrieval.cpp
    Review.cpp
    Session.cpp
    Worker.cpp
)
add_library(Morpheus::turn ALIAS morpheus_turn) # The name host projects link.
target_include_directories(morpheus_turn PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context)
morpheus_target_defaults(morpheus_turn)
```

Replace the whole of `src/llama/CMakeLists.txt` with:

```cmake
add_library(morpheus_llama STATIC
    LlamaCppBackend.cpp
)
add_library(Morpheus::llama ALIAS morpheus_llama) # The name host projects link.
target_include_directories(morpheus_llama PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE llama
)
morpheus_target_defaults(morpheus_llama)
```

In `src/app/CMakeLists.txt`, replace

```cmake
    model/SettingsEdit.cpp
)
target_include_directories(morpheus_app_model PUBLIC ${PROJECT_SOURCE_DIR}/src)
```

with

```cmake
    model/SettingsEdit.cpp
)
add_library(Morpheus::app_model ALIAS morpheus_app_model) # The name host projects link.
target_include_directories(morpheus_app_model PUBLIC ${PROJECT_SOURCE_DIR}/src)
```

and replace

```cmake
    ui/ModelSeatWindow.cpp
)
target_include_directories(morpheus_app_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
```

with

```cmake
    ui/ModelSeatWindow.cpp
)
add_library(Morpheus::app_ui ALIAS morpheus_app_ui) # The name host projects link.
target_include_directories(morpheus_app_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
```

Run: `grep -rn -w sqlite3 CMakeLists.txt src/*/CMakeLists.txt`
Expected: exactly two lines of the root `CMakeLists.txt`, the comment `# of Morpheus's own so that it never meets a host project's sqlite3 target.` and `add_library(morpheus_sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)` (matched by its source path). No target is named `sqlite3`.

- [ ] **Step 6: Run the consumer check to see it pass**

Run:

```bash
cmake -S cmake/consumer-check -B build/consumer-check -G "Visual Studio 17 2022" -A x64 && cmake --build build/consumer-check && ./build/consumer-check/Debug/consumer_check.exe; echo "exit $?"
```

Expected (PASS): the configure ends with `-- Generating done`; the build compiles `morpheus_sqlite3`, `morpheus_core`, `morpheus_inference`, `morpheus_context`, `morpheus_store`, `morpheus_turn` and `consumer_check` with no warnings and no errors; the program prints `consumer_check: ok (rolled <1-20>, total <roll + 3>)`; `exit 0`.

Run:

```bash
test -e build/consumer-check/lib && echo "lib dir exists" || echo "no lib dir"
test -e build/consumer-check/bin && echo "bin dir exists" || echo "no bin dir"
ls build/consumer-check/_deps/morpheus-build/src/core/Debug/
grep -c 'TreatWarningAsError>true' build/consumer-check/_deps/morpheus-build/src/core/morpheus_core.vcxproj
grep -c 'WarningLevel>Level4' build/consumer-check/_deps/morpheus-build/src/core/morpheus_core.vcxproj
```

Expected (PASS: the host decides where binaries go, and Morpheus's warnings are on but not errors when vendored): `no lib dir`, `no bin dir`, a listing that contains `morpheus_core.lib`, then `0`, then `4`.

- [ ] **Step 7: Run the clash check to see it pass**

Run:

```bash
cmake -S build/clash-check/src -B build/clash-check/out -G "Visual Studio 17 2022" -A x64 \
  -DHOST_JSON_SOURCE_DIR="$(pwd -W)/build/tests/_deps/nlohmann-src" -DMORPHEUS_SOURCE_DIR="$(pwd -W)"; echo "exit $?"
test -e build/clash-check/out/_deps/nlohmann-src && echo "Morpheus fetched its own nlohmann" || echo "host nlohmann kept"
cmake --build build/clash-check/out --target morpheus_store; echo "exit $?"
```

Expected (PASS): the configure ends with `-- Generating done` and `exit 0`; `host nlohmann kept`; `morpheus_store` (with `morpheus_core` and `morpheus_sqlite3`) builds against the host's nlohmann/json; `exit 0`.

Run: `rm -rf build/clash-check`

- [ ] **Step 8: The tests preset still passes**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: the build has no warnings (it builds `morpheus_sqlite3` in place of `sqlite3`); `100% tests passed, 0 tests failed out of 315`.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt src/core/CMakeLists.txt src/store/CMakeLists.txt src/inference/CMakeLists.txt src/context/CMakeLists.txt src/turn/CMakeLists.txt src/llama/CMakeLists.txt src/app/CMakeLists.txt cmake/consumer-check/CMakeLists.txt cmake/consumer-check/main.cpp
git commit -m "Make Morpheus consumable: Morpheus:: aliases, no target clashes, a consumer check" -m "SQLite's target is morpheus_sqlite3, nlohmann/json is fetched only when the host has none, and each engine library has a Morpheus:: alias. cmake/consumer-check includes Morpheus as a host would and fails if the vendored build defines more than the engine libraries." -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Linux presets and the Windows-only test

`CMakePresets.json` gains `linux-tests` and `linux-release` with matching build and test presets, shown only on Linux; the Windows presets are shown only on Windows. The one test that depends on Windows file sharing is compiled only on Windows.

**Files:**
- Modify: `CMakePresets.json` (whole file)
- Modify: `tests/app/SettingsEditTests.cpp:104-119`
- Test: `cmake --list-presets`, `ctest --preset tests`

**Interfaces:**
- Consumes (Task 1): `MORPHEUS_GPU_BACKEND`; `dev` sets `Vulkan`, `dev-asan` sets `CPU`.
- Produces: configure, build and test presets `linux-tests` (binary dir `build/linux-tests`) and `linux-release` (binary dir `build/linux-release`); Task 4's README and the owner's Linux acceptance use them.

- [ ] **Step 1: See the Linux presets missing**

Run: `cmake --list-presets`
Expected (before): `Available configure presets:` followed by `"dev"`, `"tests"`, `"asan"` and `"dev-asan"` with their display names.

Run: `cmake --preset linux-tests; echo "exit $?"`
Expected (FAIL): `CMake Error: No such preset in D:/MorpheusEngine: "linux-tests"` and `exit 1`.

- [ ] **Step 2: Write the presets**

Replace the whole of `CMakePresets.json` with:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "dev",
      "displayName": "App + tests",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build/dev",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "ON",
        "MORPHEUS_BUILD_TESTS": "ON",
        "MORPHEUS_BUILD_LLAMA": "ON",
        "MORPHEUS_GPU_BACKEND": "Vulkan"
      }
    },
    {
      "name": "tests",
      "displayName": "Engine libraries + tests only (no llama.cpp or SDL3)",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "OFF",
        "MORPHEUS_BUILD_LLAMA": "OFF"
      }
    },
    {
      "name": "asan",
      "displayName": "Tests under AddressSanitizer",
      "inherits": "tests",
      "binaryDir": "${sourceDir}/build/asan",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_ASAN": "ON"
      }
    },
    {
      "name": "dev-asan",
      "displayName": "llama.cpp on the CPU + tests under AddressSanitizer",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/dev-asan",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "OFF",
        "MORPHEUS_GPU_BACKEND": "CPU",
        "MORPHEUS_ASAN": "ON",
        "GGML_OPENMP": "OFF"
      }
    },
    {
      "name": "linux-tests",
      "displayName": "Linux: engine libraries + tests only (no llama.cpp or SDL3)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/linux-tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++",
        "MORPHEUS_BUILD_APP": "OFF",
        "MORPHEUS_BUILD_LLAMA": "OFF",
        "MORPHEUS_BUILD_TESTS": "ON"
      }
    },
    {
      "name": "linux-release",
      "displayName": "Linux: app + tools + tests, llama.cpp with CUDA (needs the CUDA toolkit)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/linux-release",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++",
        "MORPHEUS_BUILD_APP": "ON",
        "MORPHEUS_BUILD_LLAMA": "ON",
        "MORPHEUS_BUILD_TESTS": "ON",
        "MORPHEUS_GPU_BACKEND": "CUDA",
        "CMAKE_CUDA_ARCHITECTURES": "native"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "tests",
      "configurePreset": "tests",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "dev-asan",
      "configurePreset": "dev-asan",
      "configuration": "RelWithDebInfo",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "linux-tests",
      "configurePreset": "linux-tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" }
    },
    {
      "name": "linux-release",
      "configurePreset": "linux-release",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" }
    }
  ],
  "testPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "tests",
      "configurePreset": "tests",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "dev-asan",
      "configurePreset": "dev-asan",
      "configuration": "RelWithDebInfo",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "linux-tests",
      "configurePreset": "linux-tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "linux-release",
      "configurePreset": "linux-release",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    }
  ]
}
```

- [ ] **Step 3: Check the presets on Windows**

Run: `cmake --list-presets=all`
Expected: the file parses (no `CMake Error`); the configure, build and test lists each show exactly `"dev"`, `"tests"`, `"asan"` and `"dev-asan"`, and no `linux-` preset.

Run: `cmake --preset linux-tests; echo "exit $?"`
Expected (PASS: the preset exists but is disabled here): `CMake Error: Could not use disabled preset "linux-tests"` and `exit 1`.

Run: `cmake --preset tests`
Expected: configures as before (`-- Generating done`).

- [ ] **Step 4: Compile the Windows-only test only on Windows**

In `tests/app/SettingsEditTests.cpp`, replace

```cpp
TEST_CASE("SaveSettingsEdit reports a file it cannot replace and leaves no check file", "[settings_edit]")
{
```

with

```cpp
// Windows only: the test needs a replace that fails, and only Windows refuses to replace a file that another handle
// holds open without delete sharing. On Linux the rename replaces the file regardless, so the save succeeds and there
// is no failure to report.
#ifdef _WIN32
TEST_CASE("SaveSettingsEdit reports a file it cannot replace and leaves no check file", "[settings_edit]")
{
```

and replace

```cpp
    holder.close();
    CHECK(test::ReadFile(file) == before);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
}
```

with

```cpp
    holder.close();
    CHECK(test::ReadFile(file) == before);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
}
#endif
```

(That second block occurs once: it is the end of this test case, just before `TEST_CASE("ReadSettingsEdit names the file when it cannot be read", ...)`.)

- [ ] **Step 5: The test still runs on Windows**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit]"`
Expected: `All tests passed (<n> assertions in 7 test cases)`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 315`. (On Linux the same suite has 314; that is the owner's check in Task 4.)

- [ ] **Step 6: Commit**

```bash
git add CMakePresets.json tests/app/SettingsEditTests.cpp
git commit -m "Add the linux-tests and linux-release presets" -m "Each OS lists only its own presets. The test that relies on Windows refusing to replace an open file is compiled only on Windows." -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Documentation, the full Windows check, the owner's Linux acceptance

The README, CLAUDE.md and the architecture spec describe the backend choice, the Linux presets and the vendored build; every Windows preset and the consumer check run; the Linux checks are listed for the owner.

**Files:**
- Modify: `README.md`, `CLAUDE.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`

**Interfaces:**
- Consumes: everything above (option and preset names, `cmake/consumer-check/`, the aliases).
- Produces: documentation only.

- [ ] **Step 1: The README**

In `README.md`, replace

````
```
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

## Models and settings
````

with

````
```
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

llama.cpp's GPU backend is `MORPHEUS_GPU_BACKEND`: `Vulkan` (the default, used by `dev`), `CUDA` (needs the CUDA toolkit) or `CPU` (used by `dev-asan`). Any other value fails the configure.

## Models and settings
````

Replace

```
Measured on 2026-11-05 with the Linux build below:
```

with

```
Measured on 2026-11-05 with a Linux build, llama.cpp on Vulkan:
```

Replace the whole "Build (Linux)" section, README lines 87-97 today: from the line `## Build (Linux)` through the paragraph that starts `Inside a container, Vulkan sees only the CPU` and ends `... it relies on Windows refusing to replace a file another process holds open.`, up to (not including) the blank line before `## Play in the app`. The new text:

````
## Build (Linux)

Tested on Ubuntu 24.04 with clang and Ninja. On Linux, `cmake --list-presets` shows two presets (and on Windows only the Windows ones):

| Preset | Builds |
|---|---|
| `linux-tests` | Engine libraries and tests only (no llama.cpp or SDL3), Debug |
| `linux-release` | Everything, Release: llama.cpp with CUDA for the GPU in the machine (`MORPHEUS_GPU_BACKEND` `CUDA`, `CMAKE_CUDA_ARCHITECTURES` `native`), the `morpheus` app, `morpheus_probe`, `morpheus_play` and the tests |

Packages for `linux-tests`: `clang`, `cmake`, `ninja-build`.

```
cmake --preset linux-tests
cmake --build --preset linux-tests
ctest --preset linux-tests
```

`linux-release` also needs:

- the CUDA toolkit: NVIDIA's `cuda-toolkit` package from its Ubuntu 24.04 repository, with `nvcc` on `PATH` (usually `/usr/local/cuda/bin`), the NVIDIA driver, and `g++` (`nvcc` compiles CUDA host code with it);
- `pkg-config`, `libomp-dev`, `fonts-dejavu-core` (the app's font on Linux);
- SDL3's X11 and Wayland development libraries: `libx11-dev`, `libxext-dev`, `libxrandr-dev`, `libxcursor-dev`, `libxi-dev`, `libxss-dev`, `libxfixes-dev`, `libxtst-dev`, `libxkbcommon-dev`, `libwayland-dev`, `wayland-protocols`, `libegl-dev`, `libgl-dev`, `libdrm-dev`, `libgbm-dev`, `libdecor-0-dev`, `libdbus-1-dev`, `libudev-dev`.

```
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/bin/morpheus .
```

To run llama.cpp on Vulkan instead, configure with `cmake --preset linux-release -DMORPHEUS_GPU_BACKEND=Vulkan` and install `libvulkan-dev`, `glslc` and `spirv-headers` too. Inside a container, Vulkan sees only the CPU (llvmpipe) unless the host's NVIDIA user-space driver is visible: link the host's `libnvidia-*`, `libGLX_nvidia*`, `libEGL_nvidia*` and `libcuda*` libraries into a folder, put it on `LD_LIBRARY_PATH`, and point `VK_DRIVER_FILES` at an ICD file whose `library_path` is that folder's `libGLX_nvidia.so.0`. `vulkaninfo --summary` should then list the GPU.

## Using Morpheus from another project

Another CMake project can build the engine into itself with FetchContent and link the namespaced targets:

```cmake
include(FetchContent)
FetchContent_Declare(
    morpheus
    GIT_REPOSITORY https://github.com/LoshkinOleg/MorpheusEngine.git
    GIT_TAG        <commit SHA>
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(morpheus)

target_link_libraries(my_target PRIVATE Morpheus::turn)
```

- Included this way (not the top-level project), Morpheus builds only its engine libraries, `Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context` and `Morpheus::turn`, with nlohmann/json and SQLite (as `morpheus_sqlite3`, so it never meets a `sqlite3` target of yours). `MORPHEUS_BUILD_APP`, `MORPHEUS_BUILD_TESTS` and `MORPHEUS_BUILD_LLAMA` default to off, so no llama.cpp, SDL3, ImGui or Catch2 is fetched.
- Link the `Morpheus::` names, not the internal `morpheus_*` ones. For the llama.cpp backend, `Morpheus::llama`, `set(MORPHEUS_BUILD_LLAMA ON)` (and `MORPHEUS_GPU_BACKEND`: `Vulkan`, `CUDA` or `CPU`) before `FetchContent_MakeAvailable`. `Morpheus::app_model` and `Morpheus::app_ui` exist when the app or the tests are built.
- A project that already provides `nlohmann_json::nlohmann_json` (its own FetchContent or `find_package`) keeps its copy, and Morpheus links the same target.
- Morpheus's warnings stay on but are not errors (`MORPHEUS_WARNINGS_AS_ERRORS` defaults to off when vendored), so a warning from your compiler does not fail your build. Output directories and the Visual Studio startup project are yours.

`cmake/consumer-check/` is such a project, built from this checkout (`-DMORPHEUS_SOURCE_DIR=<path>` picks another): it fails to configure if the vendored build defines anything beyond the engine libraries, and its program calls into `core` and `turn`. On Linux:

```bash
cmake -S cmake/consumer-check -B build/consumer-check -G Ninja && cmake --build build/consumer-check && ./build/consumer-check/consumer_check
```

On Windows (Git Bash), the same with the Visual Studio generator:

```bash
cmake -S cmake/consumer-check -B build/consumer-check -G "Visual Studio 17 2022" -A x64 && cmake --build build/consumer-check && ./build/consumer-check/Debug/consumer_check.exe
```

It prints `consumer_check: ok` and exits with 0.
````

In the "Layout" list, after the line

```
- `tests/`: Catch2 tests and test data
```

add

```
- `cmake/consumer-check/`: a project that includes Morpheus as another project would (see "Using Morpheus from another project")
```

Then run:

```bash
sed -i 's/(`dev` preset only)/(`dev` and `linux-release` presets only)/g' README.md
grep -n 'MORPHEUS_VULKAN\|fails: it relies on Windows\|build/linux/' README.md
```

Expected: the `grep` prints nothing.

- [ ] **Step 2: CLAUDE.md**

In `CLAUDE.md`, replace

```
## Build and test (Windows, Visual Studio 2022, CMake >= 3.25, Git Bash)
```

with

```
## Build and test (CMake >= 3.25; Windows: Visual Studio 2022, Git Bash; Linux: clang, Ninja)
```

After the `dev-asan` bullet (the line that starts with ``- `dev-asan` is `dev` with llama.cpp on the CPU``), add:

```
- `MORPHEUS_GPU_BACKEND` picks llama.cpp's GPU backend: `Vulkan` (the default; `dev`), `CUDA` or `CPU` (`dev-asan`). Any other value fails the configure.
- Linux (Ubuntu 24.04): `linux-tests` is the fast loop (`cmake --preset linux-tests`, `cmake --build --preset linux-tests`, `ctest --preset linux-tests`; binaries in `build/linux-tests/bin/`); `linux-release` builds everything with llama.cpp on CUDA (needs the CUDA toolkit). Each OS lists only its own presets.
- Included in another CMake project (not top level), Morpheus builds only the engine libraries (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::turn`), with warnings that are not errors (`MORPHEUS_WARNINGS_AS_ERRORS`) and the host's output directories. `cmake/consumer-check/` checks this (README, "Using Morpheus from another project").
```

In the "Layout" list, after the line that starts `- tests/: Catch2 tests`, add:

```
- cmake/consumer-check: a host project that includes Morpheus through FetchContent and links `Morpheus::turn`; it fails to configure if the vendored build defines more than the engine libraries.
```

Then run:

```bash
sed -i 's/(`dev` preset only)/(`dev` and `linux-release` presets only)/g' CLAUDE.md
grep -n 'MORPHEUS_VULKAN' CLAUDE.md
```

Expected: the `grep` prints nothing.

- [ ] **Step 3: The architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, replace

```
**Build:** CMake, with dependencies pulled through FetchContent and pinned to commit SHAs: llama.cpp, nlohmann/json, SDL3, ImGui (upstream's docking branch, v1.92.8-docking), the SQLite amalgamation, Catch2 v3. Warnings are treated as errors (`/W4 /permissive-`). AddressSanitizer is opt-in: the `asan` and `dev-asan` presets.
```

with

```
**Build:** CMake, with dependencies pulled through FetchContent and pinned to commit SHAs: llama.cpp, nlohmann/json, SDL3, ImGui (upstream's docking branch, v1.92.8-docking), the SQLite amalgamation, Catch2 v3. llama.cpp's GPU backend is a choice, `MORPHEUS_GPU_BACKEND`: `Vulkan` (the default), `CUDA` or `CPU`. Warnings are treated as errors (`/W4 /permissive-`, `MORPHEUS_WARNINGS_AS_ERRORS`) when Morpheus is the top-level project. Included in another project through FetchContent, it builds by default only the engine libraries (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::turn`) with nlohmann/json (unless the host provides it) and SQLite (`morpheus_sqlite3`): no llama.cpp, SDL3, ImGui or Catch2, warnings that are not errors, and the host's output directories; `cmake/consumer-check/` checks this. AddressSanitizer is opt-in: the `asan` and `dev-asan` presets. On Linux, the `linux-tests` and `linux-release` (CUDA) presets.
```

Replace

```
- `LlamaCppBackend` loads two GGUF models in-process: a chat model and an embedding model. It is built with Vulkan.
```

with

```
- `LlamaCppBackend` loads two GGUF models in-process: a chat model and an embedding model. llama.cpp is built with the GPU backend `MORPHEUS_GPU_BACKEND` names: Vulkan (the default), CUDA or the CPU.
```

- [ ] **Step 4: Run the tests preset**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Expected: no warnings; `100% tests passed, 0 tests failed out of 315`.

- [ ] **Step 5: Run the AddressSanitizer preset**

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: no warnings; `100% tests passed, 0 tests failed out of 315`, with no AddressSanitizer report.

- [ ] **Step 6: Run the dev preset and start the app**

Run: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`
Expected: every target builds in Debug with no warnings in Morpheus's own targets; `0 tests failed out of 342` (the `[llama]` cases pass or show as skipped without the model variables).

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings in Morpheus's own targets.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120; echo "exit $?"`
Expected: the window opens and the app exits; `exit 0`.

- [ ] **Step 7: Run the consumer check from scratch**

Run:

```bash
rm -rf build/consumer-check
cmake -S cmake/consumer-check -B build/consumer-check -G "Visual Studio 17 2022" -A x64 && cmake --build build/consumer-check && ./build/consumer-check/Debug/consumer_check.exe; echo "exit $?"
```

Expected: configures (no `FATAL_ERROR`), builds with no errors, prints `consumer_check: ok (rolled <1-20>, total <roll + 3>)`; `exit 0`.

- [ ] **Step 8: Commit**

```bash
git add README.md CLAUDE.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
git commit -m "Document the GPU backend choice, the Linux presets and using Morpheus from another project" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 9: Hand the Linux acceptance to the owner (not run by the implementer)**

This machine cannot run these. The implementer reports them as pending and leaves the boxes below unticked; the owner runs them on the Ubuntu 24.04 desktop (clang, Ninja, CUDA 13.4, an sm_86 GPU), from a fresh clone or pull of `master` at the repository root, with the packages the README's "Build (Linux)" lists.

- [ ] Run: `cmake --list-presets`
  Expected: `Available configure presets:` lists only `"linux-tests"` and `"linux-release"`.
- [ ] Run: `cmake --preset linux-tests && cmake --build --preset linux-tests && ctest --preset linux-tests`
  Expected: builds with clang and no warnings (warnings are errors at top level); `100% tests passed, 0 tests failed out of 314` (the Windows-only settings test is not compiled).
- [ ] Run: `nvcc --version` (expected: `Cuda compilation tools, release 13.4`), then `cmake --preset linux-release && cmake --build --preset linux-release`
  Expected: the configure finds the CUDA toolkit and reports llama.cpp's CUDA backend; `grep -E '^GGML_(CUDA|VULKAN):' build/linux-release/CMakeCache.txt` prints `GGML_CUDA:BOOL=ON` and `GGML_VULKAN:BOOL=OFF`; the build finishes and `ls build/linux-release/bin` lists `morpheus`, `morpheus_play`, `morpheus_probe` and `morpheus_tests`.
- [ ] Optional, with `models/` and `settings.json` in place: `./build/linux-release/bin/morpheus . --frames 120; echo "exit $?"` prints `exit 0`, and `ctest --preset linux-release` reports 0 failures out of 341.
- [ ] Run: `cmake -S cmake/consumer-check -B build/consumer-check -G Ninja && cmake --build build/consumer-check && ./build/consumer-check/consumer_check; echo "exit $?"`
  Expected: `consumer_check: ok (rolled <1-20>, total <roll + 3>)` and `exit 0`. This uses the system's default `c++` (g++ on Ubuntu 24.04); its warnings do not fail the build. If it fails only with g++, rerun with `CC=clang CXX=clang++` in a fresh `build/consumer-check` to tell a g++ problem from a vendoring problem.

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| `MORPHEUS_BUILD_APP`, `MORPHEUS_BUILD_TESTS`, `MORPHEUS_BUILD_LLAMA` default `PROJECT_IS_TOP_LEVEL` | 1 (Step 3) |
| `MORPHEUS_GPU_BACKEND` Vulkan/CUDA/CPU, default Vulkan, cache STRING with `STRINGS`, `FATAL_ERROR` naming the option and the allowed values | 1 (Steps 3, 8) |
| `MORPHEUS_WARNINGS_AS_ERRORS` default `PROJECT_IS_TOP_LEVEL`; warning flags always on, `/WX` / `-Werror` only with it | 1 (Steps 4, 8, 9), 2 (Step 6, vendored) |
| `MORPHEUS_ASAN` unchanged | 1 (kept as is) |
| `MORPHEUS_VULKAN` removed from presets and documents | 1 (Step 7), 4 (Steps 1, 2) |
| `GGML_VULKAN` / `GGML_CUDA` set with `FORCE` from the choice; CPU turns both off; changing it reconfigures llama.cpp | 1 (Steps 5, 8) |
| The app-needs-llama.cpp `FATAL_ERROR` stays | 1 (Step 3) |
| Vendored: only the five engine libraries plus nlohmann/json and SQLite; no llama.cpp, SDL3, ImGui or Catch2; app-model and app-ui only with the app or tests | 1 (defaults), 2 (consumer check target checks) |
| Output directories only at top level | 1 (Step 2), 2 (Step 6: no `lib`/`bin` in the host tree) |
| Visual Studio startup project only at top level | 1 (Step 6) |
| `sqlite3` renamed `morpheus_sqlite3`, linked by `src/store` | 2 (Steps 4, 5, 7) |
| nlohmann/json fetched only `if(NOT TARGET nlohmann_json::nlohmann_json)` | 2 (Steps 3, 4, 7) |
| Aliases `Morpheus::core`, `store`, `inference`, `context`, `turn`, `llama`, `app_model`, `app_ui` next to their definitions | 2 (Step 5) |
| `linux-tests` preset (Ninja, Debug, clang, app/llama OFF, tests ON, `build/linux-tests`) with build and test presets | 3 (Step 2) |
| `linux-release` preset (Ninja, Release, clang, all ON, CUDA, `CMAKE_CUDA_ARCHITECTURES` `native`, `build/linux-release`) with build and test presets | 3 (Step 2) |
| Linux presets conditioned on `${hostSystemName}` == `Linux`; Windows presets on Windows; `dev-asan` uses `MORPHEUS_GPU_BACKEND` `CPU` | 1 (Step 7), 3 (Steps 2, 3) |
| The settings test compiled only on Windows, with a comment why; README's failing-test note removed | 3 (Step 4), 4 (Step 1) |
| Consumer check: `cmake/consumer-check/` `CMakeLists.txt` + `main.cpp`; `FetchContent_Declare(morpheus SOURCE_DIR ...)` + `MakeAvailable`; `MORPHEUS_SOURCE_DIR` defaulting two levels up; the target checks; `Morpheus::turn` required; calls core and turn, returns 0; host warnings, no `-Werror` | 2 (Steps 1, 2, 6) |
| Consumer check documented in the README, Ninja and Visual Studio forms | 4 (Step 1) |
| llama.cpp URL `https://github.com/ggml-org/llama.cpp.git`, same commit, TODO removed | 1 (Steps 5, 8) |
| README: Linux section on presets, CUDA toolkit listed, failing-test note dropped, "Using Morpheus from another project", `MORPHEUS_VULKAN` mentions changed | 4 (Step 1) |
| CLAUDE.md: Linux presets and `MORPHEUS_GPU_BACKEND` | 4 (Step 2) |
| Architecture spec Build paragraph: backend choice and vendored defaults | 4 (Step 3) |
| Acceptance, Windows: `tests`, `asan`, `dev` pass; app builds; `morpheus --frames 120` exits 0 | 1 (Steps 9, 10), 4 (Steps 4-6) |
| Acceptance: consumer check configures, builds and runs with exit 0 on Windows | 2 (Step 6), 4 (Step 7) |
| Acceptance: unknown `MORPHEUS_GPU_BACKEND` fails the configure naming the allowed values | 1 (Step 8) |
| Acceptance, Linux: `linux-tests` passes; `linux-release` builds app, tools and tests with CUDA; consumer check on Linux | 4 (Step 9, the owner's manual run) |
| No CI; no `MORPHEUS_BUILD_SERVER`; no llama.cpp update | not touched |
