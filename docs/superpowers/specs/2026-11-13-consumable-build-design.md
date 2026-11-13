# Consumable, Linux-buildable build (sub-project A) - Design

Date: 2026-11-13. Source: requirement R1 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` (the Skyrim/Mantella host), with the owner's decisions. First of the sub-projects A-G that the integration was split into (A build, B schema-to-grammar library, C model access over HTTP, D hosted world and `morpheus_server`, E per-speaker memory, F host prompt pack, G small items).

## Goal

1. Morpheus can be pulled into another CMake project with FetchContent and builds only its engine libraries there, without clashing with the host's targets or failing on the host's compiler warnings.
2. Morpheus builds and passes its tests natively on Linux (Ubuntu 24.04, clang, Ninja) from presets.
3. llama.cpp's GPU backend is a choice: Vulkan, CUDA or CPU.

Building Morpheus on its own on Windows stays exactly as today.

## Owner decisions

- The GPU backend becomes a choice, `MORPHEUS_GPU_BACKEND` = `Vulkan`, `CUDA` or `CPU`, default `Vulkan`.
- Two Linux presets: `linux-tests` and `linux-release` (CUDA).
- llama.cpp moves to its new URL, `https://github.com/ggml-org/llama.cpp.git`, at the same pinned commit (`960d628f46d17ea9c10aa944b061b2341afeb3b5`, b9804). A newer llama.cpp is left to sub-project C.
- No CI for now.
- `MORPHEUS_BUILD_SERVER` is added by sub-project D, together with the server it builds.

## Changes

### Options and their defaults

In the root `CMakeLists.txt`:

| Option | Default | Meaning |
|---|---|---|
| `MORPHEUS_BUILD_APP` | `PROJECT_IS_TOP_LEVEL` | The app (needs llama.cpp; fetches SDL3) |
| `MORPHEUS_BUILD_TESTS` | `PROJECT_IS_TOP_LEVEL` | `morpheus_tests` (fetches Catch2; ImGui's core for the headless UI tests) |
| `MORPHEUS_BUILD_LLAMA` | `PROJECT_IS_TOP_LEVEL` | The llama.cpp backend, `morpheus_probe` and `morpheus_play` |
| `MORPHEUS_GPU_BACKEND` | `Vulkan` | llama.cpp's GPU backend: `Vulkan`, `CUDA` or `CPU`. A cache STRING with those three values as its `STRINGS` property; any other value is a configure-time `FATAL_ERROR` naming the option and the allowed values |
| `MORPHEUS_WARNINGS_AS_ERRORS` | `PROJECT_IS_TOP_LEVEL` | `/WX` or `-Werror` on Morpheus's own targets |
| `MORPHEUS_ASAN` | `OFF` | Unchanged |

- `MORPHEUS_VULKAN` is removed; every preset and document that set it uses `MORPHEUS_GPU_BACKEND`.
- The backend choice sets llama.cpp's own switches with `FORCE`, as today: `GGML_VULKAN` is ON only for `Vulkan` and `GGML_CUDA` only for `CUDA`. `CPU` turns both off. That way, changing the choice reconfigures the fetched llama.cpp.
- The existing `FATAL_ERROR` stays: the app needs llama.cpp.

### Vendored builds

When Morpheus is not the top-level project:

- With the defaults above it builds only `morpheus_core`, `morpheus_store`, `morpheus_inference`, `morpheus_context` and `morpheus_turn`, plus their dependencies nlohmann/json and SQLite. It fetches no llama.cpp, SDL3, ImGui or Catch2. The app-model and app-ui libraries are built only with the app or the tests, as today.
- `CMAKE_RUNTIME_OUTPUT_DIRECTORY`, `CMAKE_LIBRARY_OUTPUT_DIRECTORY` and `CMAKE_ARCHIVE_OUTPUT_DIRECTORY` are set only when Morpheus is the top-level project, so the host decides where binaries go.
- The Visual Studio startup project is set only at top level.

### No target clashes

- **SQLite:** the target `sqlite3` is renamed `morpheus_sqlite3`; `src/store` links that.
- **nlohmann/json:** it is fetched only `if(NOT TARGET nlohmann_json::nlohmann_json)`. A host that already provides the library (its own FetchContent or `find_package`) keeps its copy, and Morpheus links the same target.
- **Aliases:** each engine library gets a namespaced alias next to its definition, so a host links `Morpheus::turn` and never depends on the internal names:
  - `Morpheus::core`
  - `Morpheus::store`
  - `Morpheus::inference`
  - `Morpheus::context`
  - `Morpheus::turn`
  - `Morpheus::llama` (when built)
  - `Morpheus::app_model` and `Morpheus::app_ui` (when built)

### Warnings

`morpheus_target_defaults` keeps the warning flags on every build. It adds `/WX` (MSVC) or `-Werror` (others) only when `MORPHEUS_WARNINGS_AS_ERRORS` is on. A host's newer or different compiler can then report a Morpheus warning without failing the host's build. Morpheus's own builds still fail on any warning.

### Linux presets

`CMakePresets.json` gains two configure presets, with matching build and test presets. Both are hidden on non-Linux hosts with a `condition` on `${hostSystemName}` equal to `Linux`.

- **`linux-tests`:**
  - generator `Ninja`, `CMAKE_BUILD_TYPE` `Debug`, `CMAKE_C_COMPILER` `clang`, `CMAKE_CXX_COMPILER` `clang++`;
  - `MORPHEUS_BUILD_APP` OFF, `MORPHEUS_BUILD_LLAMA` OFF, `MORPHEUS_BUILD_TESTS` ON;
  - binary dir `build/linux-tests`.

  This is the fast loop: engine libraries and tests, no llama.cpp or SDL3.
- **`linux-release`:**
  - generator `Ninja`, `CMAKE_BUILD_TYPE` `Release`, clang;
  - `MORPHEUS_BUILD_APP`, `MORPHEUS_BUILD_LLAMA` and `MORPHEUS_BUILD_TESTS` ON;
  - `MORPHEUS_GPU_BACKEND` `CUDA`, `CMAKE_CUDA_ARCHITECTURES` `native`;
  - binary dir `build/linux-release`.

  It needs the CUDA toolkit; the README lists the packages.

The Windows presets (`dev`, `tests`, `asan`, `dev-asan`) are unchanged except that `dev-asan` sets `MORPHEUS_GPU_BACKEND` to `CPU` instead of `MORPHEUS_VULKAN` OFF. They gain a `condition` restricting them to Windows, so a Linux `cmake --list-presets` shows only the Linux ones.

### A green test suite on Linux

The test "SaveSettingsEdit reports a file it cannot replace" (`tests/app/SettingsEditTests.cpp`) relies on Windows refusing to replace a file another process holds open. It is compiled only on Windows (`#ifdef _WIN32`), with a comment saying why. The README's note that this test fails on Linux goes away.

### Consumer check

The project `cmake/consumer-check/` checks the vendored build. It has a `CMakeLists.txt` and a `main.cpp`.

- It includes Morpheus with `FetchContent_Declare(morpheus SOURCE_DIR <path>)` followed by `FetchContent_MakeAvailable(morpheus)`. The path comes from the cache variable `MORPHEUS_SOURCE_DIR`, defaulting to the repository root two levels up. This is the way a host would include Morpheus, but from the local checkout.
- After `MakeAvailable`, it fails the configure (`FATAL_ERROR`) if any of these targets exist: `morpheus`, `morpheus_tests`, `morpheus_llama`, `imgui_core`, `SDL3::SDL3`, `Catch2::Catch2`. It also fails if `Morpheus::turn` does not exist.
- `main.cpp` links `Morpheus::turn`. It calls one function from `core` and one from `turn`, such as loading the mini test world's settings-free pieces or rendering a mechanics line, and returns 0. This proves the headers and the link work.
- It is built with the host's ordinary warnings and no `-Werror`.

How to run it (documented in the README):

```bash
cmake -S cmake/consumer-check -B build/consumer-check -G Ninja && cmake --build build/consumer-check && ./build/consumer-check/consumer_check
```

On Windows, the same with the Visual Studio generator.

### llama.cpp URL

At `CMakeLists.txt:84` the repository becomes `https://github.com/ggml-org/llama.cpp.git`. The pinned commit stays the same, and the TODO comment is removed.

## Documentation

- **README:**
  - The "Build (Linux)" section uses the presets (`cmake --preset linux-tests`, `cmake --preset linux-release`) instead of the long command line.
  - It lists the CUDA toolkit for `linux-release`.
  - It drops the failing-test note.
  - A short "Using Morpheus from another project" section covers FetchContent, the aliases, the vendored defaults and the consumer check.
  - Anything that mentions `MORPHEUS_VULKAN` changes to `MORPHEUS_GPU_BACKEND`.
- **CLAUDE.md:** the build section mentions the Linux presets and `MORPHEUS_GPU_BACKEND`.
- **Architecture spec:** its Build paragraph mentions the GPU backend choice and the vendored defaults.

## Acceptance

- On Windows, `cmake --preset tests`, `cmake --build --preset tests` and `ctest --preset tests` pass as today, and the same for `asan` and `dev`. The app builds and `morpheus --frames 120` exits 0.
- On Linux (Ubuntu 24.04, clang, Ninja):
  - `cmake --preset linux-tests && cmake --build --preset linux-tests && ctest --preset linux-tests` passes with no failures;
  - `cmake --preset linux-release && cmake --build --preset linux-release` builds the app, the tools and the tests with CUDA.

  The owner runs the Linux checks on the desktop; this Windows machine cannot.
- The consumer check configures, builds and runs, exiting 0, on Windows. It should do the same on Linux.
- An unknown `MORPHEUS_GPU_BACKEND` value fails the configure with a message naming the allowed values.

## Testing note

These are build-system changes: their tests are the configure, build and test runs above, plus the consumer check, not new Catch2 cases. The one Catch2 change is guarding the Windows-only test.
