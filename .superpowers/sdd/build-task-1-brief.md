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

