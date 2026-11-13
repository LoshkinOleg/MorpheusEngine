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
