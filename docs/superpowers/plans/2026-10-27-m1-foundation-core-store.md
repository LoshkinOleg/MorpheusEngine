# Milestone 1: Foundation, Core and Store - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the model-free foundation of the C++ engine.
- Build setup: CMake presets, Catch2, AddressSanitizer.
- `core` library: world loading, schema-typed state, change validation, dice.
- `store` library: one SQLite database per run.
- The sandcrawler world converted to the new world format.

**Architecture:** `core` and `store` are static libraries under `src/`, and all their tests run without a model, a window or a GPU. The existing prototype `src/main.cpp` keeps building behind `MORPHEUS_BUILD_APP` and is replaced in milestone 4. Spec: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`.

**Tech Stack:** C++20 with MSVC (Visual Studio 2022). CMake 3.25+ and FetchContent: nlohmann/json 3.12.0, the SQLite 3.50.4 amalgamation, Catch2 v3.16.1.

**Milestone roadmap** (each milestone gets its own plan, written when the previous one is done):
1. **This plan:** build foundation, `core`, `store`, sandcrawler world format.
2. **Inference and context:** `LlamaCppBackend` (tokenize, prefix KV reuse, grammar sampling, embeddings, save/restore state) and the `context` module (layered prompt, budgets).
3. **Turn:** resolve/apply/narrate orchestration, retrieval, folding, the worker thread with command/event queues, and replay tests with `ScriptedBackend`.
4. **App:** SDL3 + ImGui frontend with the Play, State, Context, Turn log, Memory and Settings windows. Replaces `src/main.cpp`.

## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25.
- **Dependencies:** fetched with FetchContent, pinned to a commit SHA (git) or a URL plus SHA256. The only new dependencies in this milestone are nlohmann/json `55f93686c01528224f448c19128836e7df245f72` (3.12.0, already used), the SQLite amalgamation `https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip` (SHA256 `1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032`), and Catch2 `08092139210881f01a34e60c5f2fd6afa0ea4024` (v3.16.1).
- **Compile flags:** every MorpheusEngine target calls `morpheus_target_defaults()`, which sets `/W4 /WX /permissive- /Zc:preprocessor /utf-8`. Never apply these flags to fetched dependencies.
- **Naming:** namespaces are `morpheus::core` and `morpheus::store`. Library targets are `morpheus_core` and `morpheus_store`. Includes are written as `#include "core/X.h"`.
- **Error policy:**
  - Engine bugs throw `morpheus::core::ContractError`, via `MORPHEUS_REQUIRE`.
  - Bad world files or saved data throw `morpheus::core::DataError` or `morpheus::store::StoreError`, naming the file and field.
  - Mistakes in model-proposed changes are never thrown. They come back as `RejectedChange` values with a reason.
- **World folder:** exactly `world.json`, `rules.md`, `lore.csv`, `state_schema.json`, `initial_state.json`.
- **Run database tables:** exactly `run_meta`, `turns`, `state_snapshots`, `summaries`, `notes`, `embeddings`.
- **Coding rules:** follow `CLAUDE.md` (created in Task 1). Source comments and docs are ASCII only.
- **Shell:** every command below runs from the repository root in Git Bash.

## File Structure

```
CMakeLists.txt                       modify: options, presets support, dependency split, engine libs, tests
CMakePresets.json                    create: dev / tests / asan presets
CLAUDE.md                            create: project rules for agents (coding style, build, layout)
README.md                            rewrite: C++ project overview
.gitignore                           modify: ignore saves/
src/core/CMakeLists.txt              create: morpheus_core library
src/core/Errors.h                    create: ContractError, DataError, MORPHEUS_REQUIRE
src/core/ContentHash.h/.cpp          create: FNV-1a 64 fingerprint of world files
src/core/Csv.h/.cpp                  create: RFC 4180 parser for lore.csv
src/core/StateSchema.h/.cpp          create: state_schema.json model, field validation, defaults
src/core/Changes.h/.cpp              create: Change, ParseChange, ApplyChanges (set/add/remove/create)
src/core/Dice.h/.cpp                 create: SplitMix64, DiceRule, CheckSeed, RollCheck
src/core/World.h/.cpp                create: World, LoadWorld
src/store/CMakeLists.txt             create: morpheus_store library
src/store/Sqlite.h/.cpp              create: RAII Database / Statement, StoreError
src/store/RunStore.h/.cpp            create: per-run database API
tests/CMakeLists.txt                 create: morpheus_tests (globs tests/**/*.cpp)
tests/TestPaths.h                    create: temp dirs, test data paths
tests/core/*Tests.cpp                create: one file per core unit
tests/store/RunStoreTests.cpp        create: store tests
tests/data/worlds/mini/*             create: tiny valid world for tests
game_projects/sandcrawler/*          convert to the new world format
```

---

### Task 1: Build foundation, test harness, project docs

**Files:**
- Modify: `CMakeLists.txt` (full rewrite below)
- Create: `CMakePresets.json`, `src/core/CMakeLists.txt`, `src/core/Errors.h`, `src/core/ContentHash.h`, `src/core/ContentHash.cpp`, `tests/CMakeLists.txt`, `tests/TestPaths.h`, `tests/core/ContentHashTests.cpp`, `CLAUDE.md`
- Modify: `README.md` (full rewrite), `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - CMake function `morpheus_target_defaults(target)`.
  - Target `morpheus_core`.
  - Test executable `morpheus_tests`, which globs `tests/**/*.cpp`, so later tasks only add files.
  - `core/Errors.h`:
    - `morpheus::core::ContractError : std::logic_error`
    - `morpheus::core::DataError : std::runtime_error`
    - macro `MORPHEUS_REQUIRE(condition, message)`
  - `core/ContentHash.h`: `class ContentHash { void Add(std::string_view); std::uint64_t Value() const; std::string Hex() const; }`
  - `tests/TestPaths.h`:
    - `morpheus::test::FreshTempDir(std::string_view)`
    - `TestDataDir()`
    - `RepoDir()`
    - `WriteFile(path, std::string_view)`

- [ ] **Step 1: Replace `CMakeLists.txt`**

The llama, SDL3 and ImGui blocks and the `morpheus` target are moved unchanged behind `MORPHEUS_BUILD_APP`.

```cmake
cmake_minimum_required(VERSION 3.25)  # 3.25+ for FetchContent SYSTEM (keeps dependency headers out of /W4 /WX)

project(MorpheusEngine
    VERSION 1 # Major only notation: 1, 2, 3, 56, 103, etc.
    LANGUAGES C CXX # C is needed for the SQLite amalgamation.
)

# ---- Global settings ----------------------------------------------------
set(CMAKE_CXX_EXTENSIONS OFF) # Enforce strict standard syntax, no compiler specific lingo.

# Put all binaries in one place - handy on Windows for DLL discovery
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For executables: .exe, .dll
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For shared libs: .so on linux
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # For static libs: .a on linux, .lib on win

# ---- Options ------------------------------------------------------------
option(MORPHEUS_BUILD_APP "Build the morpheus app (fetches llama.cpp, SDL3 and ImGui)" ON)
option(MORPHEUS_BUILD_TESTS "Build morpheus_tests (fetches Catch2)" ON)
option(MORPHEUS_ASAN "Instrument every target with AddressSanitizer (MSVC only)" OFF)

# AddressSanitizer is applied to every target, dependencies included: MSVC refuses to link instrumented and
# non-instrumented objects that share STL containers (LNK2038 'annotate_vector' mismatch).
if(MORPHEUS_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "MORPHEUS_ASAN is only wired up for MSVC.")
    endif()
    add_compile_options(/fsanitize=address)
    # MSVC's default Debug runtime checks (/RTC1) cannot be combined with /fsanitize=address.
    set(CMAKE_MSVC_RUNTIME_CHECKS "")
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

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

# ---- Dependencies (FetchContent) ---------------------------------------
include(FetchContent)

# nlohmann (engine libraries and app)
FetchContent_Declare(
    nlohmann                                                      # prefix for CMake variables
    GIT_REPOSITORY https://github.com/nlohmann/json.git           # repo
    GIT_TAG        55f93686c01528224f448c19128836e7df245f72       # commit SHA to get, 3.12.0
    GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
    SYSTEM                                                        # Treat its headers as external (no /W4 /WX)
)
FetchContent_MakeAvailable(nlohmann)

if(MORPHEUS_BUILD_APP)
    # llama
    FetchContent_Declare(
        llama                                                         # prefix for CMake variables
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git     # repo TODO: redirect to the new repo: ggml-org/llama.cpp , probably update the targeted commit?
        GIT_TAG        960d628f46d17ea9c10aa944b061b2341afeb3b5       # commit SHA to get, version b9804
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
    )
    FetchContent_MakeAvailable(llama)

    # SDL3
    FetchContent_Declare(
        sdl3                                                          # prefix for CMake variables
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git          # repo
        GIT_TAG        8e37db5e797b6167f3a00d697d816a684bd259c7       # commit SHA to get, SDL3 3.4.10
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
    )
    FetchContent_MakeAvailable(sdl3)

    # imgui
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/LoshkinOleg/cmake-ready-imgui-sdl3.git
        GIT_TAG        5acd6de3ed4efb06d665628b01fba0fe3e63f7bc
        GIT_SHALLOW    FALSE # Has to be FALSE to be able to use a commit SHA
    )
    FetchContent_MakeAvailable(imgui)
endif()

if(MORPHEUS_BUILD_TESTS)
    # Catch2
    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        08092139210881f01a34e60c5f2fd6afa0ea4024       # commit SHA to get, v3.16.1
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM
    )
    FetchContent_MakeAvailable(catch2)
endif()

# ---- Engine libraries ---------------------------------------------------
add_subdirectory(src/core)

# ---- App (prototype, replaced in milestone 4) --------------------------
if(MORPHEUS_BUILD_APP)
    add_executable(morpheus
        src/main.cpp
    )

    # For morpheus cpp and h
    target_include_directories(morpheus
        PRIVATE # No sharing with other targets
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # Warning flags
    if(MSVC)
        target_compile_options(morpheus PRIVATE /W4 /permissive- /Zc:preprocessor) # /W4 is Warning level, /permissive- is for standard compliance, /Zc:preprocessor is for MSVC preprocessor compliance
    else()
        target_compile_options(morpheus PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow) # -Wall is for the common warnings set, -Wextra is more warnings, -Wpedantic is for even more warnings, -Wconversion for warnings on narrowing casts, -Wshadow for shadowing
    endif()

    # C++20 only for morpheus (build fails on llama with C++20)
    target_compile_features(morpheus PRIVATE cxx_std_20)

    # Link morpheus
    target_link_libraries(morpheus PRIVATE
        llama
        SDL3::SDL3
        nlohmann_json::nlohmann_json
        imgui::imgui
    )

    # ---- Defines ----
    target_compile_definitions(morpheus PRIVATE
        MORPHEUS_MODELS_DIR="${CMAKE_SOURCE_DIR}/third_party/models"
    )
endif()

# ---- Tests --------------------------------------------------------------
if(MORPHEUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [ ] **Step 2: Create `CMakePresets.json`**

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
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "ON",
        "MORPHEUS_BUILD_TESTS": "ON"
      }
    },
    {
      "name": "tests",
      "displayName": "Engine libraries + tests only (no llama.cpp, SDL3 or ImGui)",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/tests",
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "OFF"
      }
    },
    {
      "name": "asan",
      "displayName": "Tests under AddressSanitizer",
      "inherits": "tests",
      "binaryDir": "${sourceDir}/build/asan",
      "cacheVariables": {
        "MORPHEUS_ASAN": "ON"
      }
    }
  ],
  "buildPresets": [
    { "name": "dev", "configurePreset": "dev", "configuration": "Debug" },
    { "name": "tests", "configurePreset": "tests", "configuration": "Debug" },
    { "name": "asan", "configurePreset": "asan", "configuration": "Debug" }
  ],
  "testPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "configuration": "Debug",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "tests",
      "configurePreset": "tests",
      "configuration": "Debug",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "configuration": "Debug",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    }
  ]
}
```

- [ ] **Step 3: Create `src/core/Errors.h`**

```cpp
#pragma once

#include <stdexcept>
#include <string>

namespace morpheus::core {

// Thrown when engine code breaks one of its own invariants. Always a bug in the engine, never bad input.
class ContractError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

// Thrown when world files or saved data are malformed. The message names the file and field at fault.
class DataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace morpheus::core

// Fails fast and loud on a broken engine contract: throws ContractError carrying `message`.
#define MORPHEUS_REQUIRE(condition, message)                                   \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw ::morpheus::core::ContractError(std::string(message));       \
        }                                                                      \
    } while (false)
```

- [ ] **Step 4: Create `src/core/CMakeLists.txt`**

```cmake
add_library(morpheus_core STATIC
    ContentHash.cpp
)
target_include_directories(morpheus_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_core PUBLIC nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_core)
```

- [ ] **Step 5: Create `tests/CMakeLists.txt` and `tests/TestPaths.h`**

`tests/CMakeLists.txt`:

```cmake
# Every *.cpp under tests/ is a test source. CONFIGURE_DEPENDS re-globs on each build, so new files are picked up.
file(GLOB_RECURSE MORPHEUS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(morpheus_tests ${MORPHEUS_TEST_SOURCES})
target_link_libraries(morpheus_tests PRIVATE morpheus_core Catch2::Catch2WithMain)
target_include_directories(morpheus_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(morpheus_tests PRIVATE
    MORPHEUS_REPO_DIR="${PROJECT_SOURCE_DIR}"
    MORPHEUS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
morpheus_target_defaults(morpheus_tests)

# The ASan runtime DLL ships next to the MSVC compiler. Copy it beside the test executable so the tests also
# run outside a Visual Studio developer prompt.
if(MORPHEUS_ASAN)
    get_filename_component(MORPHEUS_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB MORPHEUS_ASAN_DLLS "${MORPHEUS_MSVC_BIN_DIR}/clang_rt.asan*dynamic-x86_64.dll")
    if(NOT MORPHEUS_ASAN_DLLS)
        message(FATAL_ERROR "No clang_rt.asan*dynamic-x86_64.dll found in ${MORPHEUS_MSVC_BIN_DIR}.")
    endif()
    add_custom_command(TARGET morpheus_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MORPHEUS_ASAN_DLLS} $<TARGET_FILE_DIR:morpheus_tests>
        VERBATIM)
endif()

# Register every Catch2 test case with CTest. PRE_TEST discovery runs at ctest time, after the ASan DLL copy.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(morpheus_tests DISCOVERY_MODE PRE_TEST)
```

`tests/TestPaths.h`:

```cpp
#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>

namespace morpheus::test {

// A clean, empty directory under the system temp folder, one per name. Wiped first if it already exists.
inline std::filesystem::path FreshTempDir(std::string_view name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "morpheus_tests" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// tests/data in the source tree.
inline std::filesystem::path TestDataDir()
{
    return MORPHEUS_TEST_DATA_DIR;
}

// Repository root in the source tree.
inline std::filesystem::path RepoDir()
{
    return MORPHEUS_REPO_DIR;
}

// Overwrites a file with exactly `text` (binary mode: no newline translation).
inline void WriteFile(const std::filesystem::path& file, std::string_view text)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << text;
}

} // namespace morpheus::test
```

- [ ] **Step 6: Write the failing test `tests/core/ContentHashTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "core/ContentHash.h"

using morpheus::core::ContentHash;

TEST_CASE("ContentHash matches FNV-1a 64 reference values", "[hash]")
{
    // Reference vectors from the FNV specification.
    CHECK(ContentHash().Hex() == "cbf29ce484222325");

    ContentHash a;
    a.Add("a");
    CHECK(a.Hex() == "af63dc4c8601ec8c");

    ContentHash foobar;
    foobar.Add("foobar");
    CHECK(foobar.Hex() == "85944171f73967e8");
}

TEST_CASE("ContentHash is streaming: chunks hash like their concatenation", "[hash]")
{
    ContentHash chunked;
    chunked.Add("foo");
    chunked.Add("bar");

    ContentHash whole;
    whole.Add("foobar");

    CHECK(chunked.Value() == whole.Value());
}
```

- [ ] **Step 7: Configure and build. Expect failure.**

Run: `cmake --preset tests`
Expected: nlohmann and Catch2 are fetched, then configuration FAILS with `Cannot find source file: ContentHash.cpp` (from `src/core/CMakeLists.txt`). This confirms the dependencies resolve and the library is wired up, with nothing implemented yet.

- [ ] **Step 8: Implement `src/core/ContentHash.h` and `src/core/ContentHash.cpp`**

`src/core/ContentHash.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace morpheus::core {

// 64-bit FNV-1a. Fingerprints world content so a saved run can tell when its world files changed.
class ContentHash {
public:
    static constexpr std::uint64_t OFFSET_BASIS = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t PRIME = 0x100000001b3ULL;

private:
    std::uint64_t _value = OFFSET_BASIS;

public:
    // Feeds more bytes. Hashing "foo" then "bar" equals hashing "foobar".
    void Add(std::string_view bytes);

    std::uint64_t Value() const;

    // 16 lower-case hex digits.
    std::string Hex() const;
};

} // namespace morpheus::core
```

`src/core/ContentHash.cpp`:

```cpp
#include "core/ContentHash.h"

#include <format>

namespace morpheus::core {

void ContentHash::Add(std::string_view bytes)
{
    // FNV-1a: xor the byte in first, then multiply by the prime.
    for (const char c : bytes) {
        _value ^= static_cast<std::uint8_t>(c);
        _value *= PRIME;
    }
}

std::uint64_t ContentHash::Value() const
{
    return _value;
}

std::string ContentHash::Hex() const
{
    return std::format("{:016x}", _value);
}

} // namespace morpheus::core
```

- [ ] **Step 9: Build and run the tests. Expect them to pass.**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 2`.

- [ ] **Step 10: Create `CLAUDE.md`**

```markdown
# MorpheusEngine

Local-first, LLM-driven text RPG engine in C++20. The repository owner acts as product designer; Claude implements.

## Design documents
- Architecture: docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
- Narrative visualization: docs/superpowers/specs/2026-10-27-narrative-visualization-design.md
- Narration TTS: docs/superpowers/specs/2026-10-27-narration-tts-design.md
- Implementation plans: docs/superpowers/plans/

## Build and test (Windows, Visual Studio 2022, CMake >= 3.25, Git Bash)
- Fast loop (engine libraries + tests, no llama.cpp/SDL/ImGui): `cmake --preset tests`, `cmake --build --preset tests`, `ctest --preset tests`
- AddressSanitizer: the same three commands with preset `asan`
- Everything including the app: preset `dev`
- One tag or test case: `./build/tests/bin/Debug/morpheus_tests.exe "[csv]"`

## Layout
- src/core: world loading, schema-typed state, state changes, dice. No LLM, no UI; its only I/O is reading world folders.
- src/store: one SQLite database per run.
- src/main.cpp: prototype app, replaced in milestone 4.
- tests/: Catch2 tests (every tests/**/*.cpp is compiled in), tests/data: test worlds.
- game_projects/<id>/: worlds as data (world.json, rules.md, lore.csv, state_schema.json, initial_state.json).

## Coding rules
- Fail fast and loud. Engine bugs throw core::ContractError via MORPHEUS_REQUIRE. Malformed world files or saved data throw core::DataError / store::StoreError naming the file and field. Mistakes in LLM output are not exceptions: they are rejected with a reason, logged, and the turn continues. Never add fallbacks that hide a broken contract.
- Method bodies carry enough comments to read top to bottom without guessing intent.
- Extract a private helper only when it has more than one caller. Single-use logic stays inline; local lambdas are fine for organisation. Public single-use methods are fine.
- Class layout order: nested types, public data, private data, public methods, private methods, helpers.
- Constants (constexpr, static const) are ALL_CAPS and grouped together.
- Naming: types and methods PascalCase; locals and parameters camelCase; members _camelCase; raw pointers pName / _pName.
- Initialize members at declaration, even to their default value.
- RAII for every resource. No naked new/delete. Raw pointers never own.
- Prefer methods over getters when a value is computed rather than stored.
- When changing code, don't keep legacy paths for old behavior; update callers to the new contract.
- ASCII only in source comments and docs. docs/ holds documentation only.
- Each module is a static library and dependencies only flow downward (see the architecture spec).
- New behavior lands with Catch2 tests, written first.
```

- [ ] **Step 11: Rewrite `README.md`**

````markdown
# Morpheus Engine

A local-first, LLM-driven text RPG engine in C++20. The engine owns the game (structured world state, dice, memory); a local model running in-process through llama.cpp proposes outcomes and narrates them.

The earlier C# implementation lives on the `master` branch. This branch (`cpp`) is the rewrite.

## Design

- [Architecture](docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md)
- [Narrative visualization](docs/superpowers/specs/2026-10-27-narrative-visualization-design.md)
- [Narration text-to-speech](docs/superpowers/specs/2026-10-27-narration-tts-design.md)
- [Implementation plans](docs/superpowers/plans)

## Build (Windows)

Prerequisites: Visual Studio 2022 with the C++ workload, CMake 3.25+, Git. CMake fetches every dependency at a pinned commit.

| Preset | Builds |
|---|---|
| `tests` | Engine libraries and tests only (no llama.cpp, SDL3 or ImGui) |
| `asan` | Same as `tests`, instrumented with AddressSanitizer |
| `dev` | Everything, including the `morpheus` app |

```
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

## Layout

- `src/core`: world loading, schema-typed state, state changes, dice
- `src/store`: one SQLite database per run
- `src/main.cpp`: prototype app (replaced in milestone 4)
- `tests/`: Catch2 tests and test data
- `game_projects/<id>/`: worlds as data
````

- [ ] **Step 12: Ignore run saves in `.gitignore`**

Add this line directly after the existing `game_projects/*/saved/` line:

```
saves/
```

- [ ] **Step 13: Commit**

```bash
git add CMakeLists.txt CMakePresets.json CLAUDE.md README.md .gitignore src/core tests
git commit -m "Add build foundation, Catch2 harness and ContentHash" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: RFC 4180 CSV parser

**Files:**
- Create: `src/core/Csv.h`, `src/core/Csv.cpp`, `tests/core/CsvTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataError` (Task 1).
- Produces: `std::vector<std::vector<std::string>> morpheus::core::ParseCsv(std::string_view text)`.

- [ ] **Step 1: Write the failing test `tests/core/CsvTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "core/Csv.h"
#include "core/Errors.h"

using morpheus::core::DataError;
using morpheus::core::ParseCsv;
using Catch::Matchers::ContainsSubstring;
using Rows = std::vector<std::vector<std::string>>;

TEST_CASE("ParseCsv splits plain records and fields", "[csv]")
{
    CHECK(ParseCsv("a,b\nc,d\n") == Rows{{"a", "b"}, {"c", "d"}});
}

TEST_CASE("ParseCsv accepts CRLF endings and a missing final newline", "[csv]")
{
    CHECK(ParseCsv("a,b\r\nc,d") == Rows{{"a", "b"}, {"c", "d"}});
}

TEST_CASE("ParseCsv keeps empty fields", "[csv]")
{
    CHECK(ParseCsv("a,,c\n,\n") == Rows{{"a", "", "c"}, {"", ""}});
}

TEST_CASE("ParseCsv handles quoted fields", "[csv]")
{
    // Commas, doubled quotes and newlines inside quotes all belong to the field.
    CHECK(ParseCsv("\"x, y\",\"say \"\"hi\"\"\"\n\"line1\nline2\",z\n") ==
          Rows{{"x, y", "say \"hi\""}, {"line1\nline2", "z"}});
}

TEST_CASE("ParseCsv skips blank lines", "[csv]")
{
    CHECK(ParseCsv("a\n\n\r\nb\n") == Rows{{"a"}, {"b"}});
}

TEST_CASE("ParseCsv rejects malformed quoting with a line number", "[csv]")
{
    CHECK_THROWS_AS(ParseCsv("\"unterminated\n"), DataError);
    CHECK_THROWS_WITH(ParseCsv("a,b\nc\"d,e\n"), ContainsSubstring("line 2"));
    CHECK_THROWS_AS(ParseCsv("\"quoted\"tail\n"), DataError);
    CHECK_THROWS_AS(ParseCsv("a\rb\n"), DataError);
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Csv.h'`.

- [ ] **Step 3: Implement `src/core/Csv.h` and `src/core/Csv.cpp`, and register the source**

`src/core/Csv.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace morpheus::core {

// Parses RFC 4180 CSV text into records of fields.
// - Fields may be quoted; quoted fields can hold commas, newlines and doubled quotes ("" -> ").
// - Records end with LF or CRLF; the last record does not need a newline.
// - Blank lines are skipped, so a trailing newline never yields an empty record.
// Throws DataError with a 1-based line number on an unterminated quote, a quote inside an unquoted field,
// text after a closing quote, or a bare carriage return.
std::vector<std::vector<std::string>> ParseCsv(std::string_view text);

} // namespace morpheus::core
```

`src/core/Csv.cpp`:

```cpp
#include "core/Csv.h"

#include <format>

#include "core/Errors.h"

namespace morpheus::core {

std::vector<std::vector<std::string>> ParseCsv(std::string_view text)
{
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> record;
    std::string field;
    bool inQuotes = false;       // Inside a quoted field: commas and newlines are literal.
    bool fieldWasQuoted = false; // Current field was quoted: only ',' or a line end may follow the closing quote.
    bool recordStarted = false;  // Current line has content, so the next line end completes a record.
    std::size_t line = 1;        // 1-based, for error messages.

    auto endField = [&]() {
        record.push_back(std::move(field));
        field.clear();
        fieldWasQuoted = false;
    };
    auto endRecord = [&]() {
        endField();
        records.push_back(std::move(record));
        record.clear();
        recordStarted = false;
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (inQuotes) {
            if (c == '"') {
                // A doubled quote is an escaped quote; a single one closes the field.
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                if (c == '\n') {
                    ++line;
                }
                field.push_back(c);
            }
            continue;
        }

        switch (c) {
        case '"':
            if (!field.empty() || fieldWasQuoted) {
                throw DataError(std::format("CSV line {}: quote inside an unquoted field", line));
            }
            inQuotes = true;
            fieldWasQuoted = true;
            recordStarted = true;
            break;
        case ',':
            endField();
            recordStarted = true;
            break;
        case '\r':
            // Only valid as the first half of CRLF; the '\n' that follows ends the record.
            if (i + 1 >= text.size() || text[i + 1] != '\n') {
                throw DataError(std::format("CSV line {}: bare carriage return", line));
            }
            break;
        case '\n':
            // Blank lines are skipped rather than turned into empty records.
            if (recordStarted) {
                endRecord();
            }
            ++line;
            break;
        default:
            if (fieldWasQuoted) {
                throw DataError(std::format("CSV line {}: text after a closing quote", line));
            }
            field.push_back(c);
            recordStarted = true;
            break;
        }
    }

    if (inQuotes) {
        throw DataError(std::format("CSV line {}: unterminated quoted field", line));
    }
    if (recordStarted) {
        endRecord();
    }
    return records;
}

} // namespace morpheus::core
```

Replace `src/core/CMakeLists.txt` with:

```cmake
add_library(morpheus_core STATIC
    ContentHash.cpp
    Csv.cpp
)
target_include_directories(morpheus_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_core PUBLIC nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_core)
```

- [ ] **Step 4: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass (2 hash + 6 csv).

- [ ] **Step 5: Commit**

```bash
git add src/core tests/core/CsvTests.cpp
git commit -m "Add RFC 4180 CSV parser" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: State schema

**Files:**
- Create: `src/core/StateSchema.h`, `src/core/StateSchema.cpp`, `tests/core/StateSchemaTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataError`, `ContractError`, `MORPHEUS_REQUIRE` (Task 1).
- Produces (namespace `morpheus::core`):
  - `enum class FieldType { Int, Enum, String, List, Counter }`
  - `std::string FieldTypeName(FieldType)`
  - `struct FieldDef { FieldType type; int min; int max; std::vector<std::string> values; nlohmann::json defaultValue; }`
  - `struct EntityTypeDef { bool creatable; std::map<std::string, FieldDef> fields; }`
  - `class StateSchema`:
    - `static StateSchema FromJson(const nlohmann::json&)`
    - `static bool IsValidEntityId(const std::string&)`
    - `static std::string CheckFieldValue(const FieldDef&, const nlohmann::json&)`
    - `const EntityTypeDef* FindEntityType(const std::string&) const`
    - `nlohmann::json DefaultFields(const std::string&) const`
    - `void ValidateState(const nlohmann::json&) const`
- **State JSON shape** (used by every later task): `{"<entity id>": {"type": "<entity type>", "fields": {"<field>": <value>}}}`.

- [ ] **Step 1: Write the failing test `tests/core/StateSchemaTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/StateSchema.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

namespace {

nlohmann::json SchemaJson()
{
    return nlohmann::json::parse(R"({
        "entity_types": {
            "character": {
                "fields": {
                    "health": {"type": "int", "min": 0, "max": 10, "default": 10},
                    "mood": {"type": "enum", "values": ["calm", "tense"]},
                    "location": {"type": "string"},
                    "conditions": {"type": "list"},
                    "inventory": {"type": "counter"}
                }
            },
            "npc": {"creatable": true, "fields": {"health": {"type": "int", "min": 0, "max": 5}}}
        }
    })");
}

nlohmann::json ValidState()
{
    return nlohmann::json::parse(R"({
        "player": {"type": "character", "fields": {
            "health": 7, "mood": "calm", "location": "deck", "conditions": ["tired"], "inventory": {"rope": 1}}}
    })");
}

// The schema JSON with one extra field "bad" on the character type.
nlohmann::json SchemaWithBadField(const nlohmann::json& field)
{
    nlohmann::json json = SchemaJson();
    json["entity_types"]["character"]["fields"]["bad"] = field;
    return json;
}

} // namespace

TEST_CASE("StateSchema parses entity types and fields", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());

    const EntityTypeDef* pCharacter = schema.FindEntityType("character");
    REQUIRE(pCharacter != nullptr);
    CHECK_FALSE(pCharacter->creatable);
    CHECK(pCharacter->fields.at("health").type == FieldType::Int);
    CHECK(pCharacter->fields.at("health").max == 10);
    CHECK(pCharacter->fields.at("mood").values == std::vector<std::string>{"calm", "tense"});
    CHECK(pCharacter->fields.at("inventory").type == FieldType::Counter);

    REQUIRE(schema.FindEntityType("npc") != nullptr);
    CHECK(schema.FindEntityType("npc")->creatable);
    CHECK(schema.FindEntityType("dragon") == nullptr);
}

TEST_CASE("StateSchema rejects malformed definitions", "[schema]")
{
    CHECK_THROWS_AS(StateSchema::FromJson(nlohmann::json::object()), DataError);
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "float"}})), ContainsSubstring("'bad'"));
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 5}, {"max", 1}})), DataError);
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "enum"}, {"values", nlohmann::json::array()}})), DataError);
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 0}, {"max", 3}, {"default", 9}})), DataError);
}

TEST_CASE("StateSchema builds default fields", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());

    CHECK(schema.DefaultFields("character") == nlohmann::json::parse(R"({
        "health": 10, "mood": "calm", "location": "", "conditions": [], "inventory": {}
    })"));
    CHECK(schema.DefaultFields("npc") == nlohmann::json::parse(R"({"health": 0})"));
    CHECK_THROWS_AS(schema.DefaultFields("dragon"), ContractError);
}

TEST_CASE("CheckFieldValue explains invalid values", "[schema]")
{
    FieldDef health;
    health.type = FieldType::Int;
    health.min = 0;
    health.max = 10;
    CHECK(StateSchema::CheckFieldValue(health, 4).empty());
    CHECK_THAT(StateSchema::CheckFieldValue(health, 11), ContainsSubstring("0..10"));
    CHECK_FALSE(StateSchema::CheckFieldValue(health, "4").empty());

    FieldDef conditions;
    conditions.type = FieldType::List;
    CHECK(StateSchema::CheckFieldValue(conditions, nlohmann::json::array({"a", "b"})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(conditions, nlohmann::json::array({"a", "a"})).empty());

    FieldDef inventory;
    inventory.type = FieldType::Counter;
    CHECK(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 2}})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 0}})).empty());
}

TEST_CASE("ValidateState accepts valid state and names what is wrong otherwise", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());
    CHECK_NOTHROW(schema.ValidateState(ValidState()));

    nlohmann::json missingField = ValidState();
    missingField["player"]["fields"].erase("inventory");
    CHECK_THROWS_WITH(schema.ValidateState(missingField), ContainsSubstring("inventory"));

    nlohmann::json extraField = ValidState();
    extraField["player"]["fields"]["wings"] = 2;
    CHECK_THROWS_WITH(schema.ValidateState(extraField), ContainsSubstring("wings"));

    nlohmann::json badValue = ValidState();
    badValue["player"]["fields"]["health"] = 99;
    CHECK_THROWS_WITH(schema.ValidateState(badValue), ContainsSubstring("health"));

    nlohmann::json badType = ValidState();
    badType["player"]["type"] = "dragon";
    CHECK_THROWS_WITH(schema.ValidateState(badType), ContainsSubstring("dragon"));

    nlohmann::json badId = ValidState();
    badId["Bad.Id"] = badId["player"];
    CHECK_THROWS_AS(schema.ValidateState(badId), DataError);
}

TEST_CASE("Entity ids never contain the path separator", "[schema]")
{
    CHECK(StateSchema::IsValidEntityId("player"));
    CHECK(StateSchema::IsValidEntityId("npc:old_marta"));
    CHECK_FALSE(StateSchema::IsValidEntityId(""));
    CHECK_FALSE(StateSchema::IsValidEntityId("a.b"));
    CHECK_FALSE(StateSchema::IsValidEntityId("Player"));
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/StateSchema.h'`.

- [ ] **Step 3: Implement `src/core/StateSchema.h`**

```cpp
#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace morpheus::core {

enum class FieldType { Int, Enum, String, List, Counter };

// Schema spelling of a field type: "int", "enum", "string", "list" or "counter".
std::string FieldTypeName(FieldType type);

// One field of an entity type, as declared in state_schema.json.
struct FieldDef {
    FieldType type = FieldType::String;
    int min = 0;                           // Int only.
    int max = 0;                           // Int only.
    std::vector<std::string> values = {};  // Enum only, in declaration order.
    nlohmann::json defaultValue = nullptr; // Optional "default"; null means the type's natural default.
};

struct EntityTypeDef {
    bool creatable = false; // Whether model-proposed "create" changes may make new entities of this type.
    std::map<std::string, FieldDef> fields = {};
};

// The world's state definition (state_schema.json). State itself is plain JSON shaped as
// {"<entity id>": {"type": "<entity type>", "fields": {"<field>": <value>, ...}}, ...}.
// Field values: int -> integer in [min, max]; enum -> one of values; string -> string;
// list -> array of unique non-empty strings; counter -> object of item -> positive integer.
class StateSchema {
private:
    std::map<std::string, EntityTypeDef> _entityTypes = {};

public:
    // Parses state_schema.json ({"entity_types": {...}}). Throws DataError naming the type or field at fault.
    static StateSchema FromJson(const nlohmann::json& json);

    // Entity ids are non-empty and use only [a-z0-9_:], so they never contain the '.' path separator.
    static bool IsValidEntityId(const std::string& id);

    // Checks one value against its field definition. Returns "" when valid, otherwise a human-readable reason.
    static std::string CheckFieldValue(const FieldDef& def, const nlohmann::json& value);

    // nullptr when the type does not exist.
    const EntityTypeDef* FindEntityType(const std::string& typeName) const;

    // Every field of the type at its default: the declared "default", else int -> min, enum -> first value,
    // string -> "", list -> [], counter -> {}. Throws ContractError for an unknown type.
    nlohmann::json DefaultFields(const std::string& typeName) const;

    // Throws DataError unless every entity has a valid id, a known type and exactly its type's fields, all valid.
    void ValidateState(const nlohmann::json& state) const;
};

} // namespace morpheus::core
```

- [ ] **Step 4: Implement `src/core/StateSchema.cpp`, and add `StateSchema.cpp` to `src/core/CMakeLists.txt` after `Csv.cpp`**

```cpp
#include "core/StateSchema.h"

#include <algorithm>
#include <format>
#include <set>

#include "core/Errors.h"

namespace morpheus::core {

std::string FieldTypeName(FieldType type)
{
    switch (type) {
    case FieldType::Int:
        return "int";
    case FieldType::Enum:
        return "enum";
    case FieldType::String:
        return "string";
    case FieldType::List:
        return "list";
    case FieldType::Counter:
        return "counter";
    }
    throw ContractError("FieldTypeName: unhandled FieldType");
}

StateSchema StateSchema::FromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("entity_types") || !json.at("entity_types").is_object() ||
        json.at("entity_types").empty()) {
        throw DataError("state schema: needs a non-empty 'entity_types' object");
    }

    // Entity type and field names are identifiers: lower-case letters, digits and underscores.
    auto isIdentifier = [](const std::string& name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    };

    StateSchema schema;
    for (const auto& [typeName, typeJson] : json.at("entity_types").items()) {
        const std::string where = std::format("state schema: entity type '{}'", typeName);
        if (!isIdentifier(typeName)) {
            throw DataError(where + ": name must use only [a-z0-9_]");
        }
        if (!typeJson.is_object()) {
            throw DataError(where + ": must be an object");
        }

        EntityTypeDef typeDef;
        if (typeJson.contains("creatable")) {
            if (!typeJson.at("creatable").is_boolean()) {
                throw DataError(where + ": 'creatable' must be true or false");
            }
            typeDef.creatable = typeJson.at("creatable").get<bool>();
        }
        if (!typeJson.contains("fields") || !typeJson.at("fields").is_object() || typeJson.at("fields").empty()) {
            throw DataError(where + ": needs a non-empty 'fields' object");
        }

        for (const auto& [fieldName, fieldJson] : typeJson.at("fields").items()) {
            const std::string fieldWhere = std::format("{}: field '{}'", where, fieldName);
            if (!isIdentifier(fieldName)) {
                throw DataError(fieldWhere + ": name must use only [a-z0-9_]");
            }
            if (!fieldJson.is_object() || !fieldJson.contains("type") || !fieldJson.at("type").is_string()) {
                throw DataError(fieldWhere + ": needs a string 'type'");
            }

            // Type-specific attributes: int needs a range, enum needs its values.
            FieldDef def;
            const std::string typeText = fieldJson.at("type").get<std::string>();
            if (typeText == "int") {
                def.type = FieldType::Int;
                if (!fieldJson.contains("min") || !fieldJson.at("min").is_number_integer() ||
                    !fieldJson.contains("max") || !fieldJson.at("max").is_number_integer()) {
                    throw DataError(fieldWhere + ": int fields need integer 'min' and 'max'");
                }
                def.min = fieldJson.at("min").get<int>();
                def.max = fieldJson.at("max").get<int>();
                if (def.min > def.max) {
                    throw DataError(fieldWhere + ": 'min' is greater than 'max'");
                }
            } else if (typeText == "enum") {
                def.type = FieldType::Enum;
                if (!fieldJson.contains("values") || !fieldJson.at("values").is_array() ||
                    fieldJson.at("values").empty()) {
                    throw DataError(fieldWhere + ": enum fields need a non-empty 'values' array");
                }
                std::set<std::string> seen;
                for (const nlohmann::json& value : fieldJson.at("values")) {
                    if (!value.is_string() || value.get<std::string>().empty() ||
                        !seen.insert(value.get<std::string>()).second) {
                        throw DataError(fieldWhere + ": enum values must be unique non-empty strings");
                    }
                    def.values.push_back(value.get<std::string>());
                }
            } else if (typeText == "string") {
                def.type = FieldType::String;
            } else if (typeText == "list") {
                def.type = FieldType::List;
            } else if (typeText == "counter") {
                def.type = FieldType::Counter;
            } else {
                throw DataError(std::format("{}: unknown type '{}' (expected int, enum, string, list or counter)",
                                            fieldWhere, typeText));
            }

            // An explicit default must itself be a valid value.
            if (fieldJson.contains("default")) {
                const std::string problem = CheckFieldValue(def, fieldJson.at("default"));
                if (!problem.empty()) {
                    throw DataError(fieldWhere + ": invalid default: " + problem);
                }
                def.defaultValue = fieldJson.at("default");
            }
            typeDef.fields.emplace(fieldName, std::move(def));
        }
        schema._entityTypes.emplace(typeName, std::move(typeDef));
    }
    return schema;
}

bool StateSchema::IsValidEntityId(const std::string& id)
{
    return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':';
    });
}

std::string StateSchema::CheckFieldValue(const FieldDef& def, const nlohmann::json& value)
{
    switch (def.type) {
    case FieldType::Int:
        if (!value.is_number_integer()) {
            return std::format("expected an integer, got {}", value.dump());
        }
        if (value.get<long long>() < def.min || value.get<long long>() > def.max) {
            return std::format("{} is outside {}..{}", value.get<long long>(), def.min, def.max);
        }
        return {};
    case FieldType::Enum:
        if (!value.is_string() ||
            std::find(def.values.begin(), def.values.end(), value.get<std::string>()) == def.values.end()) {
            return std::format("expected one of {}, got {}", nlohmann::json(def.values).dump(), value.dump());
        }
        return {};
    case FieldType::String:
        if (!value.is_string()) {
            return std::format("expected a string, got {}", value.dump());
        }
        return {};
    case FieldType::List: {
        if (!value.is_array()) {
            return std::format("expected an array of strings, got {}", value.dump());
        }
        std::set<std::string> seen;
        for (const nlohmann::json& item : value) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return std::format("list items must be non-empty strings, got {}", item.dump());
            }
            if (!seen.insert(item.get<std::string>()).second) {
                return std::format("list item {} appears twice", item.dump());
            }
        }
        return {};
    }
    case FieldType::Counter:
        if (!value.is_object()) {
            return std::format("expected an object of item counts, got {}", value.dump());
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!it.value().is_number_integer() || it.value().get<long long>() <= 0) {
                return std::format("count of '{}' must be a positive integer, got {}", it.key(), it.value().dump());
            }
        }
        return {};
    }
    throw ContractError("CheckFieldValue: unhandled FieldType");
}

const EntityTypeDef* StateSchema::FindEntityType(const std::string& typeName) const
{
    const auto it = _entityTypes.find(typeName);
    return it == _entityTypes.end() ? nullptr : &it->second;
}

nlohmann::json StateSchema::DefaultFields(const std::string& typeName) const
{
    const EntityTypeDef* pType = FindEntityType(typeName);
    MORPHEUS_REQUIRE(pType != nullptr, std::format("DefaultFields: unknown entity type '{}'", typeName));

    nlohmann::json fields = nlohmann::json::object();
    for (const auto& [name, def] : pType->fields) {
        if (!def.defaultValue.is_null()) {
            fields[name] = def.defaultValue;
            continue;
        }
        switch (def.type) {
        case FieldType::Int:
            fields[name] = def.min;
            break;
        case FieldType::Enum:
            fields[name] = def.values.front();
            break;
        case FieldType::String:
            fields[name] = "";
            break;
        case FieldType::List:
            fields[name] = nlohmann::json::array();
            break;
        case FieldType::Counter:
            fields[name] = nlohmann::json::object();
            break;
        }
    }
    return fields;
}

void StateSchema::ValidateState(const nlohmann::json& state) const
{
    if (!state.is_object()) {
        throw DataError("state: must be an object of entities");
    }

    for (const auto& [entityId, entity] : state.items()) {
        const std::string where = std::format("state: entity '{}'", entityId);
        if (!IsValidEntityId(entityId)) {
            throw DataError(where + ": ids use only [a-z0-9_:]");
        }
        if (!entity.is_object() || !entity.contains("type") || !entity.at("type").is_string() ||
            !entity.contains("fields") || !entity.at("fields").is_object()) {
            throw DataError(where + ": must be {\"type\": ..., \"fields\": {...}}");
        }

        const std::string typeName = entity.at("type").get<std::string>();
        const EntityTypeDef* pType = FindEntityType(typeName);
        if (pType == nullptr) {
            throw DataError(std::format("{}: unknown type '{}'", where, typeName));
        }

        // Every declared field must be present and valid...
        const nlohmann::json& fields = entity.at("fields");
        for (const auto& [fieldName, def] : pType->fields) {
            if (!fields.contains(fieldName)) {
                throw DataError(std::format("{}: missing field '{}'", where, fieldName));
            }
            const std::string problem = CheckFieldValue(def, fields.at(fieldName));
            if (!problem.empty()) {
                throw DataError(std::format("{}: field '{}': {}", where, fieldName, problem));
            }
        }
        // ...and nothing undeclared may be present.
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (!pType->fields.contains(it.key())) {
                throw DataError(std::format("{}: unknown field '{}'", where, it.key()));
            }
        }
    }
}

} // namespace morpheus::core
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/core tests/core/StateSchemaTests.cpp
git commit -m "Add state schema with field validation and defaults" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: State changes (set / add / remove / create)

**Files:**
- Create: `src/core/Changes.h`, `src/core/Changes.cpp`, `tests/core/ChangesTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `StateSchema`, `FieldDef`, `FieldType`, `FieldTypeName`, `StateSchema::CheckFieldValue`, `StateSchema::IsValidEntityId`, `StateSchema::DefaultFields`, `StateSchema::FindEntityType` (Task 3).
- Produces (namespace `morpheus::core`):
  - `enum class ChangeOp { Set, Add, Remove, Create }`
  - `std::string ChangeOpName(ChangeOp)`
  - `struct Change { ChangeOp op; std::string path; nlohmann::json value; std::string reason; }`
  - `struct AppliedChange { Change change; std::string note; }`
  - `struct RejectedChange { nlohmann::json raw; std::string reason; }`
  - `struct ApplyResult { nlohmann::json state; std::vector<AppliedChange> applied; std::vector<RejectedChange> rejected; }`
  - `std::optional<Change> ParseChange(const nlohmann::json& raw, std::string& error)`
  - `nlohmann::json ChangeToJson(const Change&)`
  - `ApplyResult ApplyChanges(const StateSchema&, const nlohmann::json& state, const nlohmann::json& proposedChanges)`

- [ ] **Step 1: Write the failing test `tests/core/ChangesTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/Errors.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

namespace {

StateSchema TestSchema()
{
    return StateSchema::FromJson(nlohmann::json::parse(R"({
        "entity_types": {
            "character": {
                "fields": {
                    "health": {"type": "int", "min": 0, "max": 10},
                    "mood": {"type": "enum", "values": ["calm", "tense"]},
                    "location": {"type": "string"},
                    "conditions": {"type": "list"},
                    "inventory": {"type": "counter"}
                }
            },
            "npc": {
                "creatable": true,
                "fields": {
                    "disposition": {"type": "enum", "values": ["wary", "friendly"]},
                    "health": {"type": "int", "min": 0, "max": 5, "default": 5}
                }
            }
        }
    })"));
}

nlohmann::json TestState()
{
    return nlohmann::json::parse(R"({
        "player": {"type": "character", "fields": {
            "health": 7, "mood": "calm", "location": "deck", "conditions": ["tired"], "inventory": {"rope": 1}}}
    })");
}

// Applies one change, given as JSON text, to TestState().
ApplyResult ApplySingle(const std::string& changeJson)
{
    return ApplyChanges(TestSchema(), TestState(), nlohmann::json::array({nlohmann::json::parse(changeJson)}));
}

const nlohmann::json& PlayerField(const ApplyResult& result, const std::string& field)
{
    return result.state.at("player").at("fields").at(field);
}

} // namespace

TEST_CASE("set replaces a field with a valid value", "[changes]")
{
    const ApplyResult result = ApplySingle(R"({"op": "set", "path": "player.location", "value": "bridge"})");
    REQUIRE(result.rejected.empty());
    REQUIRE(result.applied.size() == 1);
    CHECK(result.applied.at(0).change.path == "player.location");
    CHECK(PlayerField(result, "location") == "bridge");
}

TEST_CASE("set rejects anything the schema does not allow and leaves state untouched", "[changes]")
{
    const ApplyResult outOfRange = ApplySingle(R"({"op": "set", "path": "player.health", "value": 11})");
    REQUIRE(outOfRange.rejected.size() == 1);
    CHECK_THAT(outOfRange.rejected.at(0).reason, ContainsSubstring("0..10"));
    CHECK(outOfRange.state == TestState());

    CHECK(ApplySingle(R"({"op": "set", "path": "player.mood", "value": "angry"})").rejected.size() == 1);
    CHECK_THAT(ApplySingle(R"({"op": "set", "path": "ghost.health", "value": 1})").rejected.at(0).reason,
               ContainsSubstring("unknown entity"));
    CHECK_THAT(ApplySingle(R"({"op": "set", "path": "player.wings", "value": 1})").rejected.at(0).reason,
               ContainsSubstring("no field"));
    CHECK(ApplySingle(R"({"op": "set", "path": "player.inventory.rope", "value": 3})").rejected.size() == 1);
}

TEST_CASE("add on an int field clamps to the field range", "[changes]")
{
    const ApplyResult up = ApplySingle(R"({"op": "add", "path": "player.health", "value": 5})");
    CHECK(PlayerField(up, "health") == 10);
    CHECK(up.applied.at(0).note == "clamped to 10");

    const ApplyResult down = ApplySingle(R"({"op": "add", "path": "player.health", "value": -2})");
    CHECK(PlayerField(down, "health") == 5);
    CHECK(down.applied.at(0).note.empty());
}

TEST_CASE("add and remove on a list field", "[changes]")
{
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.conditions", "value": "bruised"})"), "conditions") ==
          nlohmann::json::array({"tired", "bruised"}));

    const ApplyResult duplicate = ApplySingle(R"({"op": "add", "path": "player.conditions", "value": "tired"})");
    CHECK(PlayerField(duplicate, "conditions") == nlohmann::json::array({"tired"}));
    CHECK(duplicate.applied.at(0).note == "already present");

    CHECK(PlayerField(ApplySingle(R"({"op": "remove", "path": "player.conditions", "value": "tired"})"), "conditions") ==
          nlohmann::json::array());
    CHECK(ApplySingle(R"({"op": "remove", "path": "player.conditions", "value": "lost"})").rejected.size() == 1);
}

TEST_CASE("add and remove on counter items", "[changes]")
{
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": 2})"), "inventory") ==
          nlohmann::json({{"rope", 3}}));
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.inventory.water", "value": 1})"), "inventory") ==
          nlohmann::json({{"rope", 1}, {"water", 1}}));

    const ApplyResult usedUp = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": -1})");
    CHECK(PlayerField(usedUp, "inventory") == nlohmann::json::object());
    CHECK(usedUp.applied.at(0).note == "removed (count reached 0)");

    CHECK(ApplySingle(R"({"op": "add", "path": "player.inventory.water", "value": -1})").rejected.size() == 1);
    CHECK(PlayerField(ApplySingle(R"({"op": "remove", "path": "player.inventory.rope"})"), "inventory") ==
          nlohmann::json::object());
    CHECK(ApplySingle(R"({"op": "remove", "path": "player.inventory.gold"})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "add", "path": "player.inventory", "value": 1})").rejected.size() == 1);
}

TEST_CASE("create adds entities of creatable types, filling defaults", "[changes]")
{
    const ApplyResult result = ApplySingle(
        R"({"op": "create", "path": "npc:marta", "value": {"type": "npc", "fields": {"disposition": "friendly"}}})");
    REQUIRE(result.rejected.empty());
    CHECK(result.state.at("npc:marta") ==
          nlohmann::json::parse(R"({"type": "npc", "fields": {"disposition": "friendly", "health": 5}})"));

    CHECK_THAT(ApplySingle(R"({"op": "create", "path": "hero", "value": {"type": "character"}})").rejected.at(0).reason,
               ContainsSubstring("cannot be created"));
    CHECK_THAT(ApplySingle(R"({"op": "create", "path": "player", "value": {"type": "npc"}})").rejected.at(0).reason,
               ContainsSubstring("already exists"));
    CHECK(ApplySingle(R"({"op": "create", "path": "Bad.Id", "value": {"type": "npc"}})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "create", "path": "npc:x", "value": {"type": "npc", "fields": {"health": 9}}})")
              .rejected.size() == 1);
}

TEST_CASE("changes see the effects of earlier changes in the same batch", "[changes]")
{
    const nlohmann::json changes = nlohmann::json::parse(R"([
        {"op": "create", "path": "npc:marta", "value": {"type": "npc"}},
        {"op": "set", "path": "npc:marta.disposition", "value": "friendly"},
        {"op": "set", "path": "player.health", "value": 99},
        {"op": "set", "path": "player.mood", "value": "tense"}
    ])");
    const ApplyResult result = ApplyChanges(TestSchema(), TestState(), changes);

    CHECK(result.applied.size() == 3);
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.rejected.at(0).raw == changes.at(2));
    CHECK(result.state.at("npc:marta").at("fields").at("disposition") == "friendly");
    CHECK(result.state.at("player").at("fields").at("mood") == "tense");
}

TEST_CASE("malformed changes are rejected, not thrown", "[changes]")
{
    CHECK_THAT(ApplySingle(R"({"path": "player.health", "value": 1})").rejected.at(0).reason, ContainsSubstring("op"));
    CHECK(ApplySingle(R"({"op": "explode", "path": "player.health"})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "set", "path": "", "value": 1})").rejected.size() == 1);
    CHECK(ApplySingle(R"("not an object")").rejected.size() == 1);
}

TEST_CASE("ApplyChanges requires an array: anything else is an engine bug", "[changes]")
{
    CHECK_THROWS_AS(ApplyChanges(TestSchema(), TestState(), nlohmann::json::object()), ContractError);
}

TEST_CASE("ChangeToJson round-trips through ParseChange", "[changes]")
{
    const Change change{ChangeOp::Add, "player.health", -2, "fell"};
    std::string error;
    const std::optional<Change> parsed = ParseChange(ChangeToJson(change), error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->op == ChangeOp::Add);
    CHECK(parsed->path == "player.health");
    CHECK(parsed->value == -2);
    CHECK(parsed->reason == "fell");
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Changes.h'`.

- [ ] **Step 3: Implement `src/core/Changes.h`**

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/StateSchema.h"

namespace morpheus::core {

enum class ChangeOp { Set, Add, Remove, Create };

// Op spelling in change JSON: "set", "add", "remove" or "create".
std::string ChangeOpName(ChangeOp op);

// One proposed state change. Path forms:
// - "entity.field"       set / add / remove on a field
// - "entity.field.item"  add / remove on one item of a counter field
// - "entity"             create, with value {"type": "<creatable type>", "fields": {...optional...}}
struct Change {
    ChangeOp op = ChangeOp::Set;
    std::string path = {};
    nlohmann::json value = nullptr;
    std::string reason = {};
};

struct AppliedChange {
    Change change = {};
    std::string note = {}; // What the engine adjusted, e.g. "clamped to 0". Empty when applied exactly as proposed.
};

struct RejectedChange {
    nlohmann::json raw = nullptr; // The change exactly as proposed.
    std::string reason = {};
};

struct ApplyResult {
    nlohmann::json state = nullptr; // State after every valid change.
    std::vector<AppliedChange> applied = {};
    std::vector<RejectedChange> rejected = {};
};

// Reads one change object {"op", "path", "value"?, "reason"?}. On malformed input returns nullopt and sets `error`.
std::optional<Change> ParseChange(const nlohmann::json& raw, std::string& error);

nlohmann::json ChangeToJson(const Change& change);

// Applies proposed changes, in order, to a copy of `state`. Each change is checked against the schema and the
// state as left by the changes before it; invalid ones are rejected with a reason and skipped. Rules:
// - set:    any field to a valid value. Counter item paths are not allowed (use add/remove).
// - add:    int -> adds the integer value, clamped to [min, max] (note "clamped to N");
//           list -> appends the string if absent (note "already present" otherwise);
//           counter item -> adds a non-zero integer; at 0 or below the item is removed
//           (note "removed (count reached 0)"); decreasing an absent item is rejected.
// - remove: list -> removes the string (must be present); counter item -> removes the item (must be present).
// - create: new entity id of a creatable type; missing fields take their defaults.
// `proposedChanges` must be a JSON array (ContractError otherwise); individual malformed entries are rejected.
ApplyResult ApplyChanges(const StateSchema& schema, const nlohmann::json& state, const nlohmann::json& proposedChanges);

} // namespace morpheus::core
```

- [ ] **Step 4: Implement `src/core/Changes.cpp`, and add `Changes.cpp` to `src/core/CMakeLists.txt` after `StateSchema.cpp`**

```cpp
#include "core/Changes.h"

#include <algorithm>
#include <format>

#include "core/Errors.h"

namespace morpheus::core {

std::string ChangeOpName(ChangeOp op)
{
    switch (op) {
    case ChangeOp::Set:
        return "set";
    case ChangeOp::Add:
        return "add";
    case ChangeOp::Remove:
        return "remove";
    case ChangeOp::Create:
        return "create";
    }
    throw ContractError("ChangeOpName: unhandled ChangeOp");
}

std::optional<Change> ParseChange(const nlohmann::json& raw, std::string& error)
{
    if (!raw.is_object()) {
        error = "change must be a JSON object";
        return std::nullopt;
    }
    if (!raw.contains("op") || !raw.at("op").is_string()) {
        error = "change needs a string 'op'";
        return std::nullopt;
    }
    if (!raw.contains("path") || !raw.at("path").is_string() || raw.at("path").get<std::string>().empty()) {
        error = "change needs a non-empty string 'path'";
        return std::nullopt;
    }
    if (raw.contains("reason") && !raw.at("reason").is_string()) {
        error = "change 'reason' must be a string";
        return std::nullopt;
    }

    Change change;
    const std::string opName = raw.at("op").get<std::string>();
    if (opName == "set") {
        change.op = ChangeOp::Set;
    } else if (opName == "add") {
        change.op = ChangeOp::Add;
    } else if (opName == "remove") {
        change.op = ChangeOp::Remove;
    } else if (opName == "create") {
        change.op = ChangeOp::Create;
    } else {
        error = std::format("unknown op '{}' (expected set, add, remove or create)", opName);
        return std::nullopt;
    }
    change.path = raw.at("path").get<std::string>();
    change.value = raw.contains("value") ? raw.at("value") : nlohmann::json(nullptr);
    change.reason = raw.contains("reason") ? raw.at("reason").get<std::string>() : std::string();
    return change;
}

nlohmann::json ChangeToJson(const Change& change)
{
    nlohmann::json result = {{"op", ChangeOpName(change.op)}, {"path", change.path}};
    if (!change.value.is_null()) {
        result["value"] = change.value;
    }
    if (!change.reason.empty()) {
        result["reason"] = change.reason;
    }
    return result;
}

ApplyResult ApplyChanges(const StateSchema& schema, const nlohmann::json& state, const nlohmann::json& proposedChanges)
{
    MORPHEUS_REQUIRE(proposedChanges.is_array(), "ApplyChanges expects a JSON array of changes");

    // Applies one parsed change to `target`. Returns "" on success (and may set `note`), else the rejection reason.
    // Every check happens before `target` is modified, so a rejected change never leaves partial edits.
    auto applyOne = [&schema](nlohmann::json& target, const Change& change, std::string& note) -> std::string {
        // Path segments: entity[.field[.item]]
        std::vector<std::string> parts(1);
        for (const char c : change.path) {
            if (c == '.') {
                parts.emplace_back();
            } else {
                parts.back().push_back(c);
            }
        }

        if (change.op == ChangeOp::Create) {
            if (parts.size() != 1 || !StateSchema::IsValidEntityId(change.path)) {
                return std::format("'{}' is not a valid entity id (use [a-z0-9_:])", change.path);
            }
            if (target.contains(change.path)) {
                return std::format("entity '{}' already exists", change.path);
            }
            if (!change.value.is_object() || !change.value.contains("type") || !change.value.at("type").is_string()) {
                return "create needs a value {\"type\": ..., \"fields\": {...}}";
            }
            const std::string typeName = change.value.at("type").get<std::string>();
            const EntityTypeDef* pType = schema.FindEntityType(typeName);
            if (pType == nullptr) {
                return std::format("unknown entity type '{}'", typeName);
            }
            if (!pType->creatable) {
                return std::format("entity type '{}' cannot be created", typeName);
            }

            // Start from defaults, then overlay the proposed fields after checking each one.
            nlohmann::json fields = schema.DefaultFields(typeName);
            if (change.value.contains("fields")) {
                const nlohmann::json& given = change.value.at("fields");
                if (!given.is_object()) {
                    return "create 'fields' must be an object";
                }
                for (auto it = given.begin(); it != given.end(); ++it) {
                    const auto fieldIt = pType->fields.find(it.key());
                    if (fieldIt == pType->fields.end()) {
                        return std::format("entity type '{}' has no field '{}'", typeName, it.key());
                    }
                    const std::string problem = StateSchema::CheckFieldValue(fieldIt->second, it.value());
                    if (!problem.empty()) {
                        return std::format("field '{}': {}", it.key(), problem);
                    }
                    fields[it.key()] = it.value();
                }
            }
            target[change.path] = {{"type", typeName}, {"fields", std::move(fields)}};
            return {};
        }

        // Every other op addresses a field, optionally one item of a counter field.
        if (parts.size() < 2 || parts.size() > 3) {
            return std::format("path '{}' must be entity.field or entity.field.item", change.path);
        }
        const std::string& entityId = parts[0];
        const std::string& fieldName = parts[1];
        if (!target.contains(entityId)) {
            return std::format("unknown entity '{}'", entityId);
        }
        nlohmann::json& entity = target.at(entityId);
        const EntityTypeDef* pType = schema.FindEntityType(entity.at("type").get<std::string>());
        MORPHEUS_REQUIRE(pType != nullptr, "ApplyChanges: state holds an entity of an unknown type");
        const auto fieldIt = pType->fields.find(fieldName);
        if (fieldIt == pType->fields.end()) {
            return std::format("entity '{}' has no field '{}'", entityId, fieldName);
        }
        const FieldDef& def = fieldIt->second;
        nlohmann::json& field = entity.at("fields").at(fieldName);

        const bool itemPath = parts.size() == 3;
        if (itemPath && def.type != FieldType::Counter) {
            return std::format("only counter fields take an item path; '{}' is {}", fieldName, FieldTypeName(def.type));
        }
        if (itemPath && parts[2].empty()) {
            return "counter item name is empty";
        }

        switch (change.op) {
        case ChangeOp::Set: {
            if (itemPath) {
                return "use add or remove to change a counter item";
            }
            const std::string problem = StateSchema::CheckFieldValue(def, change.value);
            if (!problem.empty()) {
                return problem;
            }
            field = change.value;
            return {};
        }
        case ChangeOp::Add:
            if (def.type == FieldType::Int) {
                if (!change.value.is_number_integer()) {
                    return "add on an int field needs an integer value";
                }
                const long long wanted = field.get<long long>() + change.value.get<long long>();
                const long long clamped = std::clamp<long long>(wanted, def.min, def.max);
                if (clamped != wanted) {
                    note = std::format("clamped to {}", clamped);
                }
                field = clamped;
                return {};
            }
            if (def.type == FieldType::List) {
                if (!change.value.is_string() || change.value.get<std::string>().empty()) {
                    return "add on a list field needs a non-empty string value";
                }
                if (std::find(field.begin(), field.end(), change.value) != field.end()) {
                    note = "already present";
                    return {};
                }
                field.push_back(change.value);
                return {};
            }
            if (def.type == FieldType::Counter) {
                if (!itemPath) {
                    return "add on a counter field needs an item path (entity.field.item)";
                }
                if (!change.value.is_number_integer() || change.value.get<long long>() == 0) {
                    return "add on a counter item needs a non-zero integer value";
                }
                const std::string& item = parts[2];
                const long long current = field.contains(item) ? field.at(item).get<long long>() : 0;
                if (current == 0 && change.value.get<long long>() < 0) {
                    return std::format("no item '{}' to decrease", item);
                }
                const long long next = current + change.value.get<long long>();
                if (next <= 0) {
                    field.erase(item);
                    note = "removed (count reached 0)";
                } else {
                    field[item] = next;
                }
                return {};
            }
            return std::format("add is not valid on a {} field", FieldTypeName(def.type));
        case ChangeOp::Remove:
            if (def.type == FieldType::List) {
                const auto it = std::find(field.begin(), field.end(), change.value);
                if (it == field.end()) {
                    return std::format("{} is not in the list", change.value.dump());
                }
                field.erase(it);
                return {};
            }
            if (def.type == FieldType::Counter) {
                if (!itemPath) {
                    return "remove on a counter field needs an item path (entity.field.item)";
                }
                if (!field.contains(parts[2])) {
                    return std::format("no item '{}'", parts[2]);
                }
                field.erase(parts[2]);
                return {};
            }
            return std::format("remove is not valid on a {} field", FieldTypeName(def.type));
        case ChangeOp::Create:
            break; // Handled above.
        }
        throw ContractError("ApplyChanges: unhandled ChangeOp");
    };

    ApplyResult result;
    result.state = state;
    for (const nlohmann::json& raw : proposedChanges) {
        std::string error;
        const std::optional<Change> change = ParseChange(raw, error);
        if (!change) {
            result.rejected.push_back({raw, error});
            continue;
        }

        std::string note;
        const std::string problem = applyOne(result.state, *change, note);
        if (!problem.empty()) {
            result.rejected.push_back({raw, problem});
            continue;
        }
        result.applied.push_back({*change, note});
    }
    return result;
}

} // namespace morpheus::core
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/core tests/core/ChangesTests.cpp
git commit -m "Add validated state changes (set, add, remove, create)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Dice

**Files:**
- Create: `src/core/Dice.h`, `src/core/Dice.cpp`, `tests/core/DiceTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataError`, `ContractError`, `MORPHEUS_REQUIRE` (Task 1).
- Produces (namespace `morpheus::core`):
  - `class SplitMix64 { explicit SplitMix64(std::uint64_t); std::uint64_t Next(); }`
  - `struct DiceRule { int count; int sides; bool addStat; static DiceRule Parse(std::string_view); }`
  - `struct CheckResult { std::vector<int> rolls; int statValue; int total; int target; bool success; }`
  - `std::uint64_t CheckSeed(std::uint64_t runSeed, int turn, int checkIndex)`
  - `CheckResult RollCheck(const DiceRule&, int statValue, int target, std::uint64_t seed)`
- Design decision recorded here: rolls are derived from `(runSeed, turn, checkIndex)`, so regenerating a turn reproduces its rolls. Regenerate changes the prose, not the dice, which also prevents re-rolling by regenerating.

- [ ] **Step 1: Write the failing test `tests/core/DiceTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "core/Dice.h"
#include "core/Errors.h"

using namespace morpheus::core;

TEST_CASE("SplitMix64 matches its reference output", "[dice]")
{
    SplitMix64 rng(0);
    CHECK(rng.Next() == 0xe220a8397b1dcdafULL);
}

TEST_CASE("DiceRule parses NdS and NdS+stat", "[dice]")
{
    const DiceRule d20 = DiceRule::Parse("1d20+stat");
    CHECK(d20.count == 1);
    CHECK(d20.sides == 20);
    CHECK(d20.addStat);

    const DiceRule pool = DiceRule::Parse(" 3d6 ");
    CHECK(pool.count == 3);
    CHECK(pool.sides == 6);
    CHECK_FALSE(pool.addStat);

    CHECK(DiceRule::Parse("2D10 + STAT").addStat);
}

TEST_CASE("DiceRule rejects malformed expressions", "[dice]")
{
    for (const char* bad : {"", "d20", "1d", "1x20", "0d6", "21d6", "1d1", "1d20+luck", "1d20+stat+stat"}) {
        INFO(bad);
        CHECK_THROWS_AS(DiceRule::Parse(bad), DataError);
    }
}

TEST_CASE("RollCheck is deterministic per seed and adds the stat", "[dice]")
{
    const DiceRule rule = DiceRule::Parse("2d6+stat");
    const CheckResult first = RollCheck(rule, 3, 10, CheckSeed(42, 1, 0));
    const CheckResult again = RollCheck(rule, 3, 10, CheckSeed(42, 1, 0));

    CHECK(first.rolls == again.rolls);
    REQUIRE(first.rolls.size() == 2);
    CHECK(first.statValue == 3);
    CHECK(first.total == first.rolls[0] + first.rolls[1] + 3);
    CHECK(first.target == 10);
    CHECK(first.success == (first.total >= 10));
}

TEST_CASE("CheckSeed differs across runs, turns and checks", "[dice]")
{
    CHECK(CheckSeed(42, 1, 0) != CheckSeed(42, 2, 0));
    CHECK(CheckSeed(42, 1, 0) != CheckSeed(42, 1, 1));
    CHECK(CheckSeed(42, 1, 0) != CheckSeed(43, 1, 0));
    CHECK_THROWS_AS(CheckSeed(42, 0, 0), ContractError);
}

TEST_CASE("RollCheck covers every face and nothing else", "[dice]")
{
    const DiceRule d6 = DiceRule::Parse("1d6");
    std::set<int> faces;
    for (int turn = 1; turn <= 600; ++turn) {
        const CheckResult result = RollCheck(d6, 5, 1, CheckSeed(7, turn, 0));
        faces.insert(result.rolls.at(0));
        CHECK(result.statValue == 0); // "1d6" does not add the stat.
    }
    CHECK(faces == std::set<int>{1, 2, 3, 4, 5, 6});
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Dice.h'`.

- [ ] **Step 3: Implement `src/core/Dice.h`**

```cpp
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace morpheus::core {

// SplitMix64: small, fast, well-mixed 64-bit generator. Chosen over std:: engines and distributions because its
// output is identical on every compiler and standard library, which replays rely on.
class SplitMix64 {
private:
    std::uint64_t _state = 0;

public:
    explicit SplitMix64(std::uint64_t seed);

    std::uint64_t Next();
};

// The world's roll expression from world.json: "NdS" or "NdS+stat" (case and spaces ignored).
struct DiceRule {
    int count = 1;
    int sides = 20;
    bool addStat = false;

    // Throws DataError unless the expression has that shape with 1 <= N <= 20 and 2 <= S <= 1000.
    static DiceRule Parse(std::string_view expression);
};

struct CheckResult {
    std::vector<int> rolls = {};
    int statValue = 0; // 0 when the rule does not add a stat.
    int total = 0;
    int target = 0;
    bool success = false; // total >= target
};

// Seed for one check. The same (runSeed, turn, checkIndex) always gives the same seed, so a replayed or
// regenerated turn reproduces its rolls. Throws ContractError if turn < 1 or checkIndex < 0.
std::uint64_t CheckSeed(std::uint64_t runSeed, int turn, int checkIndex);

// Rolls rule.count dice with rule.sides faces, adds statValue when rule.addStat, and compares with target.
CheckResult RollCheck(const DiceRule& rule, int statValue, int target, std::uint64_t seed);

} // namespace morpheus::core
```

- [ ] **Step 4: Implement `src/core/Dice.cpp`, and add `Dice.cpp` to `src/core/CMakeLists.txt` after `Changes.cpp`**

```cpp
#include "core/Dice.h"

#include <cctype>
#include <charconv>
#include <format>
#include <string>

#include "core/Errors.h"

namespace morpheus::core {

SplitMix64::SplitMix64(std::uint64_t seed)
    : _state(seed)
{
}

std::uint64_t SplitMix64::Next()
{
    // Reference SplitMix64 (Steele, Lea, Flood 2014): golden-ratio increment followed by two xor-shift-multiply rounds.
    std::uint64_t z = (_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

DiceRule DiceRule::Parse(std::string_view expression)
{
    const std::string where = std::format("dice expression '{}'", expression);

    // Normalize: drop spaces and lower-case, so "2D10 + STAT" reads as "2d10+stat".
    std::string compact;
    for (const char c : expression) {
        if (c != ' ') {
            compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }

    DiceRule rule;
    std::string_view rest = compact;
    if (rest.ends_with("+stat")) {
        rule.addStat = true;
        rest.remove_suffix(5);
    }

    const std::size_t d = rest.find('d');
    if (d == std::string_view::npos || d == 0 || d + 1 == rest.size()) {
        throw DataError(where + ": expected NdS or NdS+stat");
    }

    // Both numbers must be plain decimal integers that consume their whole segment.
    auto parseNumber = [&where](std::string_view digits) {
        int value = 0;
        const auto [pEnd, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc() || pEnd != digits.data() + digits.size()) {
            throw DataError(where + ": expected NdS or NdS+stat");
        }
        return value;
    };
    rule.count = parseNumber(rest.substr(0, d));
    rule.sides = parseNumber(rest.substr(d + 1));

    if (rule.count < 1 || rule.count > 20) {
        throw DataError(where + ": dice count must be 1..20");
    }
    if (rule.sides < 2 || rule.sides > 1000) {
        throw DataError(where + ": die sides must be 2..1000");
    }
    return rule;
}

std::uint64_t CheckSeed(std::uint64_t runSeed, int turn, int checkIndex)
{
    MORPHEUS_REQUIRE(turn >= 1 && checkIndex >= 0, "CheckSeed: turn must be >= 1 and checkIndex >= 0");

    // Turn in the high half, check index in the low half, then one SplitMix64 step to spread the bits.
    SplitMix64 mixer(runSeed ^ (static_cast<std::uint64_t>(turn) << 32) ^ static_cast<std::uint64_t>(checkIndex));
    return mixer.Next();
}

CheckResult RollCheck(const DiceRule& rule, int statValue, int target, std::uint64_t seed)
{
    SplitMix64 rng(seed);
    CheckResult result;
    result.statValue = rule.addStat ? statValue : 0;
    result.target = target;
    result.total = result.statValue;

    // Modulo bias is below 1e-16 for these die sizes, irrelevant for gameplay.
    for (int i = 0; i < rule.count; ++i) {
        const int roll = 1 + static_cast<int>(rng.Next() % static_cast<std::uint64_t>(rule.sides));
        result.rolls.push_back(roll);
        result.total += roll;
    }
    result.success = result.total >= target;
    return result;
}

} // namespace morpheus::core
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/core tests/core/DiceTests.cpp
git commit -m "Add deterministic dice and check resolution" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: World loading

**Files:**
- Create: `src/core/World.h`, `src/core/World.cpp`, `tests/core/WorldTests.cpp`
- Create: `tests/data/worlds/mini/world.json`, `rules.md`, `lore.csv`, `state_schema.json`, `initial_state.json`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `ContentHash` (Task 1), `ParseCsv` (Task 2), `StateSchema` (Task 3), `DiceRule` (Task 5), `DataError`.
- Produces (namespace `morpheus::core`):
  - `enum class MemoryMode { Engine, Assisted }`
  - `struct LoreEntry { std::string subject; std::string data; bool alwaysOn; }`
  - `struct World { id, title, dice, difficulties, checkEntity, defaultMemoryMode, rules, lore, schema, initialState, contentHash }`
  - `World LoadWorld(const std::filesystem::path& directory)`
- **`world.json` format** (used by Task 7):
  `{"id": "<folder name>", "title": "...", "dice": "1d20+stat", "check_entity": "<entity id>", "difficulties": {"<name>": <int>}, "memory_mode": "engine" | "assisted"}`

- [ ] **Step 1: Create the test world `tests/data/worlds/mini/`**

`tests/data/worlds/mini/world.json`:

```json
{
  "id": "mini",
  "title": "Mini World",
  "dice": "1d20+stat",
  "check_entity": "player",
  "difficulties": { "easy": 8, "hard": 16 },
  "memory_mode": "engine"
}
```

`tests/data/worlds/mini/rules.md`:

```markdown
You are the GM of a tiny test world.
```

`tests/data/worlds/mini/lore.csv`:

```
subject,data,always_on
lighthouse,"A tall lighthouse, painted red.",true
keeper,"The keeper says ""hello"" to everyone.",false
storm,Storms arrive at dusk.,
```

`tests/data/worlds/mini/state_schema.json`:

```json
{
  "entity_types": {
    "character": {
      "fields": {
        "health": { "type": "int", "min": 0, "max": 10 },
        "agility": { "type": "int", "min": 0, "max": 5 },
        "location": { "type": "string" },
        "inventory": { "type": "counter" }
      }
    },
    "npc": {
      "creatable": true,
      "fields": {
        "disposition": { "type": "enum", "values": ["wary", "friendly"] }
      }
    }
  }
}
```

`tests/data/worlds/mini/initial_state.json`:

```json
{
  "player": {
    "type": "character",
    "fields": { "health": 10, "agility": 3, "location": "shore", "inventory": { "lantern": 1 } }
  }
}
```

- [ ] **Step 2: Write the failing test `tests/core/WorldTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <map>
#include <string>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/World.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

fs::path MiniWorldDir()
{
    return morpheus::test::TestDataDir() / "worlds" / "mini";
}

// Copies the mini world into a fresh temp folder so a test can break one file. `folderName` is the copy's
// folder name, which LoadWorld compares with the world id.
fs::path CopyMiniWorld(const std::string& testName, const std::string& folderName = "mini")
{
    const fs::path dir = morpheus::test::FreshTempDir(testName) / folderName;
    fs::copy(MiniWorldDir(), dir, fs::copy_options::recursive);
    return dir;
}

// world.json for the mini world with one key overridden.
std::string MiniWorldJsonWith(const std::string& key, const std::string& valueJson)
{
    nlohmann::json json = nlohmann::json::parse(R"({
        "id": "mini", "title": "Mini World", "dice": "1d20+stat", "check_entity": "player",
        "difficulties": {"easy": 8, "hard": 16}, "memory_mode": "engine"
    })");
    json[key] = nlohmann::json::parse(valueJson);
    return json.dump();
}

} // namespace

TEST_CASE("LoadWorld loads every file of a valid world", "[world]")
{
    const World world = LoadWorld(MiniWorldDir());

    CHECK(world.id == "mini");
    CHECK(world.title == "Mini World");
    CHECK(world.dice.sides == 20);
    CHECK(world.dice.addStat);
    CHECK(world.checkEntity == "player");
    CHECK(world.difficulties == std::map<std::string, int>{{"easy", 8}, {"hard", 16}});
    CHECK(world.defaultMemoryMode == MemoryMode::Engine);
    CHECK_THAT(world.rules, ContainsSubstring("tiny test world"));

    REQUIRE(world.lore.size() == 3);
    CHECK(world.lore[0].subject == "lighthouse");
    CHECK(world.lore[0].data == "A tall lighthouse, painted red.");
    CHECK(world.lore[0].alwaysOn);
    CHECK(world.lore[1].data == "The keeper says \"hello\" to everyone.");
    CHECK_FALSE(world.lore[1].alwaysOn);
    CHECK_FALSE(world.lore[2].alwaysOn);

    CHECK(world.schema.FindEntityType("npc") != nullptr);
    CHECK(world.initialState.at("player").at("fields").at("health") == 10);
    CHECK(world.contentHash.size() == 16);
}

TEST_CASE("LoadWorld content hash tracks file contents", "[world]")
{
    const fs::path dir = CopyMiniWorld("world_hash");
    const std::string before = LoadWorld(dir).contentHash;
    CHECK(LoadWorld(dir).contentHash == before);

    morpheus::test::WriteFile(dir / "rules.md", "Different rules.\n");
    CHECK(LoadWorld(dir).contentHash != before);
}

TEST_CASE("LoadWorld reports the file at fault", "[world]")
{
    SECTION("missing file")
    {
        const fs::path dir = CopyMiniWorld("world_missing");
        fs::remove(dir / "lore.csv");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("lore.csv"));
    }
    SECTION("id does not match the folder")
    {
        CHECK_THROWS_WITH(LoadWorld(CopyMiniWorld("world_id", "renamed")), ContainsSubstring("folder name"));
    }
    SECTION("malformed JSON")
    {
        const fs::path dir = CopyMiniWorld("world_json");
        morpheus::test::WriteFile(dir / "world.json", "{ not json");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("world.json"));
    }
    SECTION("unknown memory mode")
    {
        const fs::path dir = CopyMiniWorld("world_memory_mode");
        morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("memory_mode", "\"chaos\""));
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("memory_mode"));
    }
    SECTION("check entity not in the initial state")
    {
        const fs::path dir = CopyMiniWorld("world_check_entity");
        morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("check_entity", "\"hero\""));
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("check_entity"));
    }
    SECTION("bad lore header")
    {
        const fs::path dir = CopyMiniWorld("world_lore_header");
        morpheus::test::WriteFile(dir / "lore.csv", "name,text\nx,y\n");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("lore.csv"));
    }
    SECTION("duplicate lore subject")
    {
        const fs::path dir = CopyMiniWorld("world_lore_duplicate");
        morpheus::test::WriteFile(dir / "lore.csv", "subject,data\nx,one\nx,two\n");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("duplicate"));
    }
    SECTION("invalid initial state")
    {
        const fs::path dir = CopyMiniWorld("world_initial_state");
        morpheus::test::WriteFile(dir / "initial_state.json",
            R"({"player": {"type": "character", "fields": {"health": 99, "agility": 3, "location": "shore", "inventory": {}}}})");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("health"));
    }
}
```

- [ ] **Step 3: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/World.h'`.

- [ ] **Step 4: Implement `src/core/World.h`**

```cpp
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Dice.h"
#include "core/StateSchema.h"

namespace morpheus::core {

// Who manages memory beyond the engine's deterministic retrieval (architecture spec, turn flow).
enum class MemoryMode { Engine, Assisted };

struct LoreEntry {
    std::string subject = {};
    std::string data = {};
    bool alwaysOn = false; // Always in the prompt (layer L1) instead of retrieved when relevant.
};

// Everything in one game_projects/<id>/ folder, validated.
struct World {
    std::string id = {};
    std::string title = {};
    DiceRule dice = {};
    std::map<std::string, int> difficulties = {}; // Difficulty name -> target number.
    std::string checkEntity = {};                 // Entity whose int fields are the stats that checks add.
    MemoryMode defaultMemoryMode = MemoryMode::Engine;
    std::string rules = {};
    std::vector<LoreEntry> lore = {}; // In file order.
    StateSchema schema = {};
    nlohmann::json initialState = nullptr;
    std::string contentHash = {}; // FNV-1a over every world file, see LoadWorld.
};

// Loads a world folder: world.json, rules.md, lore.csv, state_schema.json, initial_state.json.
// Throws DataError naming the file (and field) at fault.
World LoadWorld(const std::filesystem::path& directory);

} // namespace morpheus::core
```

- [ ] **Step 5: Implement `src/core/World.cpp`, and add `World.cpp` to `src/core/CMakeLists.txt` after `Dice.cpp`**

```cpp
#include "core/World.h"

#include <array>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include "core/ContentHash.h"
#include "core/Csv.h"
#include "core/Errors.h"

namespace morpheus::core {

namespace {

// Reads a whole file as raw bytes. Throws DataError when it cannot be opened.
std::string ReadTextFile(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw DataError(std::format("cannot read '{}'", file.generic_string()));
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Parses JSON text, turning parse errors into DataError prefixed with the file name.
nlohmann::json ParseJsonFile(const std::filesystem::path& file, const std::string& text)
{
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        throw DataError(std::format("'{}': {}", file.generic_string(), e.what()));
    }
}

} // namespace

World LoadWorld(const std::filesystem::path& directory)
{
    // A trailing separator would leave filename() empty; normalize so the folder-name check works.
    std::filesystem::path dir = directory.lexically_normal();
    if (dir.filename().empty()) {
        dir = dir.parent_path();
    }
    const std::string folderName = dir.filename().string();

    // Read every file up front: the content hash covers all of them, names included, in this fixed order.
    const std::array<std::string, 5> FILE_NAMES = {
        "world.json", "rules.md", "lore.csv", "state_schema.json", "initial_state.json"};
    std::map<std::string, std::string> texts;
    ContentHash hash;
    for (const std::string& name : FILE_NAMES) {
        std::string text = ReadTextFile(dir / name);
        hash.Add(name);
        hash.Add(text);
        texts.emplace(name, std::move(text));
    }

    World world;
    world.contentHash = hash.Hex();

    // world.json: identity, dice, difficulties, defaults.
    const std::filesystem::path worldFile = dir / "world.json";
    const std::string worldWhere = std::format("'{}'", worldFile.generic_string());
    {
        const nlohmann::json json = ParseJsonFile(worldFile, texts.at("world.json"));
        if (!json.is_object()) {
            throw DataError(worldWhere + ": must be a JSON object");
        }
        auto requireString = [&](const char* key) {
            if (!json.contains(key) || !json.at(key).is_string() || json.at(key).get<std::string>().empty()) {
                throw DataError(std::format("{}: needs a non-empty string '{}'", worldWhere, key));
            }
            return json.at(key).get<std::string>();
        };

        world.id = requireString("id");
        if (world.id != folderName) {
            throw DataError(std::format("{}: id '{}' must match the folder name '{}'", worldWhere, world.id, folderName));
        }
        world.title = requireString("title");

        const std::string diceText = requireString("dice");
        try {
            world.dice = DiceRule::Parse(diceText);
        } catch (const DataError& e) {
            throw DataError(std::format("{}: {}", worldWhere, e.what()));
        }

        world.checkEntity = requireString("check_entity");

        const std::string memoryMode = requireString("memory_mode");
        if (memoryMode == "engine") {
            world.defaultMemoryMode = MemoryMode::Engine;
        } else if (memoryMode == "assisted") {
            world.defaultMemoryMode = MemoryMode::Assisted;
        } else {
            throw DataError(std::format("{}: memory_mode must be 'engine' or 'assisted', got '{}'", worldWhere, memoryMode));
        }

        if (!json.contains("difficulties") || !json.at("difficulties").is_object() || json.at("difficulties").empty()) {
            throw DataError(worldWhere + ": needs a non-empty 'difficulties' object");
        }
        for (const auto& [name, target] : json.at("difficulties").items()) {
            if (!target.is_number_integer()) {
                throw DataError(std::format("{}: difficulty '{}' must be an integer", worldWhere, name));
            }
            world.difficulties.emplace(name, target.get<int>());
        }
    }

    // rules.md: the GM instructions, used verbatim.
    world.rules = texts.at("rules.md");
    if (world.rules.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw DataError(std::format("'{}': must not be empty", (dir / "rules.md").generic_string()));
    }

    // lore.csv: header "subject,data" or "subject,data,always_on", then one entry per record.
    {
        const std::string loreWhere = std::format("'{}'", (dir / "lore.csv").generic_string());
        std::vector<std::vector<std::string>> records;
        try {
            records = ParseCsv(texts.at("lore.csv"));
        } catch (const DataError& e) {
            throw DataError(std::format("{}: {}", loreWhere, e.what()));
        }

        const std::vector<std::string> plainHeader = {"subject", "data"};
        const std::vector<std::string> alwaysOnHeader = {"subject", "data", "always_on"};
        if (records.empty() || (records.front() != plainHeader && records.front() != alwaysOnHeader)) {
            throw DataError(loreWhere + ": header must be subject,data or subject,data,always_on");
        }
        const bool hasAlwaysOn = records.front() == alwaysOnHeader;

        std::set<std::string> subjects;
        for (std::size_t i = 1; i < records.size(); ++i) {
            const std::vector<std::string>& record = records[i];
            const std::string recordWhere = std::format("{} record {}", loreWhere, i + 1);
            if (record.size() != records.front().size()) {
                throw DataError(std::format("{}: expected {} fields, got {}", recordWhere, records.front().size(), record.size()));
            }

            LoreEntry entry;
            entry.subject = record[0];
            entry.data = record[1];
            if (entry.subject.empty() || entry.data.empty()) {
                throw DataError(recordWhere + ": subject and data must not be empty");
            }
            if (!subjects.insert(entry.subject).second) {
                throw DataError(std::format("{}: duplicate subject '{}'", recordWhere, entry.subject));
            }
            // always_on: "true", "false" or empty (= false).
            if (hasAlwaysOn) {
                if (record[2] == "true") {
                    entry.alwaysOn = true;
                } else if (record[2] != "false" && !record[2].empty()) {
                    throw DataError(recordWhere + ": always_on must be true, false or empty");
                }
            }
            world.lore.push_back(std::move(entry));
        }
    }

    // state_schema.json, then initial_state.json validated against it.
    {
        const std::filesystem::path file = dir / "state_schema.json";
        const nlohmann::json json = ParseJsonFile(file, texts.at("state_schema.json"));
        try {
            world.schema = StateSchema::FromJson(json);
        } catch (const DataError& e) {
            throw DataError(std::format("'{}': {}", file.generic_string(), e.what()));
        }
    }
    {
        const std::filesystem::path file = dir / "initial_state.json";
        world.initialState = ParseJsonFile(file, texts.at("initial_state.json"));
        try {
            world.schema.ValidateState(world.initialState);
        } catch (const DataError& e) {
            throw DataError(std::format("'{}': {}", file.generic_string(), e.what()));
        }
    }

    // Checks add stats from this entity, so it has to exist from the start.
    if (!world.initialState.contains(world.checkEntity)) {
        throw DataError(std::format("{}: check_entity '{}' is not an entity in initial_state.json", worldWhere, world.checkEntity));
    }
    return world;
}

} // namespace morpheus::core
```

- [ ] **Step 6: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/core tests/core/WorldTests.cpp tests/data
git commit -m "Add world folder loading with content hash" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Convert the sandcrawler world

**Files:**
- Rename: `game_projects/sandcrawler/lore/default_lore_entries.csv` -> `game_projects/sandcrawler/lore.csv`
- Rename and edit: `game_projects/sandcrawler/system/instructions.md` -> `game_projects/sandcrawler/rules.md`
- Create: `game_projects/sandcrawler/world.json`, `state_schema.json`, `initial_state.json`
- Delete: `game_projects/sandcrawler/manifest.json`, `game_projects/sandcrawler/system/agent_prompt.md`
- Create: `tests/core/SandcrawlerWorldTests.cpp`

**Interfaces:**
- Consumes: `LoadWorld`, `World`, `EntityTypeDef` (Tasks 3 and 6), `morpheus::test::RepoDir()` (Task 1).
- Produces: a loadable `game_projects/sandcrawler/` world. Entity ids are `player` (the check entity), `crawler` and `world`. Created NPCs use type `npc`. The stats added to checks are the player's `grit`, `wits`, `presence` and `agility`.
- Note for the repository owner: the stats, resources and starting values below are a first pass based on the lore entries "player starting conditions" and "small sandcrawler". Tune them freely. They are data, not code.

- [ ] **Step 1: Write the failing test `tests/core/SandcrawlerWorldTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestPaths.h"
#include "core/World.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The shipped sandcrawler world loads", "[world][sandcrawler]")
{
    const World world = LoadWorld(morpheus::test::RepoDir() / "game_projects" / "sandcrawler");

    CHECK(world.id == "sandcrawler");
    CHECK(world.lore.size() == 42);
    CHECK(world.difficulties.at("hard") == 16);
    CHECK(world.checkEntity == "player");
    CHECK(world.initialState.at("crawler").at("fields").at("crew") == 20);
    CHECK_THAT(world.rules, ContainsSubstring("Second person, present tense"));

    const EntityTypeDef* pNpc = world.schema.FindEntityType("npc");
    REQUIRE(pNpc != nullptr);
    CHECK(pNpc->creatable);
}
```

- [ ] **Step 2: Build and run it. Expect failure.**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[sandcrawler]"`
Expected: FAIL with a `DataError` message containing `cannot read` and `world.json`.

- [ ] **Step 3: Move and delete the old files**

```bash
git mv game_projects/sandcrawler/lore/default_lore_entries.csv game_projects/sandcrawler/lore.csv
git mv game_projects/sandcrawler/system/instructions.md game_projects/sandcrawler/rules.md
git rm game_projects/sandcrawler/manifest.json game_projects/sandcrawler/system/agent_prompt.md
```

- [ ] **Step 4: Edit `game_projects/sandcrawler/rules.md`**

Replace the whole file with the following. Only the Lore and Adjudication paragraphs change, to match the engine: lore is supplied as entries, and the engine rolls the dice and tracks resources as state.

```markdown
You are the GM for an ongoing solo tabletop RPG set on a desert planet of nomadic sandcrawlers. The player controls one character; you control everything else.

**Lore.** Canonical lore is given to you as reference entries. Treat it as ground truth. NEVER invent facts that contradict existing lore. If unsure whether something is established, ask rather than assume.

**Pushback.** If the player attempts something that contradicts lore, physics, or established character capability, resist in-fiction first — the world doesn't bend, the NPC refuses, the action fails. Break character only if they seem genuinely confused about the rules of the world. The player character is not specially destined. NPCs have their own agendas and may dislike, ignore, or oppose the character for valid reasons.

**Narration.** Second person, present tense. Tight prose; cut purple description unless the moment earns it. End scenes on a decision point. Distinguish player knowledge from character knowledge — if the character wouldn't know it, don't reveal it.

**Adjudication.** When outcomes are uncertain, name the stakes and call for a check; the engine rolls the dice and reports the result. Don't fudge results — failure is interesting. Injuries, time, fuel, water, and ammunition persist across scenes; the engine tracks them as state.
```

- [ ] **Step 5: Create `game_projects/sandcrawler/world.json`**

```json
{
  "id": "sandcrawler",
  "title": "Sandcrawler Captain",
  "dice": "1d20+stat",
  "check_entity": "player",
  "difficulties": { "easy": 8, "medium": 12, "hard": 16, "extreme": 20 },
  "memory_mode": "engine"
}
```

- [ ] **Step 6: Create `game_projects/sandcrawler/state_schema.json`**

```json
{
  "entity_types": {
    "captain": {
      "fields": {
        "health": { "type": "int", "min": 0, "max": 10, "default": 10 },
        "conditions": { "type": "list" },
        "location": { "type": "string" },
        "inventory": { "type": "counter" },
        "grit": { "type": "int", "min": 0, "max": 5 },
        "wits": { "type": "int", "min": 0, "max": 5 },
        "presence": { "type": "int", "min": 0, "max": 5 },
        "agility": { "type": "int", "min": 0, "max": 5 }
      }
    },
    "crawler": {
      "fields": {
        "fuel": { "type": "int", "min": 0, "max": 100 },
        "water": { "type": "int", "min": 0, "max": 100 },
        "food": { "type": "int", "min": 0, "max": 100 },
        "ammunition": { "type": "int", "min": 0, "max": 100 },
        "hull": { "type": "int", "min": 0, "max": 100, "default": 100 },
        "crew": { "type": "int", "min": 0, "max": 40 },
        "morale": { "type": "enum", "values": ["steady", "mutinous", "low", "high"] },
        "location": { "type": "string" },
        "cargo": { "type": "counter" },
        "vehicles": { "type": "counter" }
      }
    },
    "world": {
      "fields": {
        "day": { "type": "int", "min": 1, "max": 100000 },
        "time_of_day": { "type": "enum", "values": ["dawn", "morning", "noon", "afternoon", "dusk", "night"] }
      }
    },
    "npc": {
      "creatable": true,
      "fields": {
        "role": { "type": "string" },
        "disposition": { "type": "enum", "values": ["neutral", "hostile", "wary", "friendly", "loyal"] },
        "location": { "type": "string" },
        "conditions": { "type": "list" },
        "health": { "type": "int", "min": 0, "max": 10, "default": 10 }
      }
    }
  }
}
```

- [ ] **Step 7: Create `game_projects/sandcrawler/initial_state.json`**

```json
{
  "player": {
    "type": "captain",
    "fields": {
      "health": 10,
      "conditions": [],
      "location": "open desert",
      "inventory": { "pistol": 1, "compass": 1 },
      "grit": 2,
      "wits": 3,
      "presence": 2,
      "agility": 1
    }
  },
  "crawler": {
    "type": "crawler",
    "fields": {
      "fuel": 60,
      "water": 50,
      "food": 50,
      "ammunition": 40,
      "hull": 100,
      "crew": 20,
      "morale": "steady",
      "location": "open desert",
      "cargo": {},
      "vehicles": { "sand_bike": 2 }
    }
  },
  "world": {
    "type": "world",
    "fields": { "day": 1, "time_of_day": "dawn" }
  }
}
```

- [ ] **Step 8: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `The shipped sandcrawler world loads`.

- [ ] **Step 9: Commit**

```bash
git add -A game_projects/sandcrawler tests/core/SandcrawlerWorldTests.cpp
git commit -m "Convert sandcrawler to the world folder format" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: SQLite wrapper and run database creation

**Files:**
- Modify: `CMakeLists.txt` (SQLite dependency, `src/store` subdirectory)
- Modify: `tests/CMakeLists.txt` (link `morpheus_store`)
- Create: `src/store/CMakeLists.txt`, `src/store/Sqlite.h`, `src/store/Sqlite.cpp`, `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `MORPHEUS_REQUIRE`, `ContractError` (Task 1).
- Produces (namespace `morpheus::store`):
  - `class StoreError : std::runtime_error`
  - `class Database`:
    - `static Database Open(const std::filesystem::path&, bool create)`
    - `void Exec(std::string_view)`
    - `Statement Prepare(std::string_view)`
    - `template<class F> void Transaction(F&&)`
  - `class Statement`:
    - `Bind(int, std::int64_t)`, `Bind(int, std::string_view)`, `Bind(int, const std::vector<float>&)` (each returns `Statement&`)
    - `bool Step()`, `void Run()`
    - `std::int64_t ColumnInt(int) const`, `std::string ColumnText(int) const`, `std::vector<float> ColumnFloats(int) const`
  - `struct RunMeta { std::string worldId, worldHash, modelId; std::uint64_t rngSeed; nlohmann::json settings; std::string createdAt; }`
  - `class RunStore`:
    - `static constexpr int SCHEMA_VERSION = 1`
    - `static RunStore Create(const std::filesystem::path&, RunMeta, const nlohmann::json& initialState)`
    - `static RunStore Open(const std::filesystem::path&)`
    - `const RunMeta& Meta() const`
    - `int LatestTurn()`
    - `nlohmann::json StateAt(int turn)`

- [ ] **Step 1: Add the SQLite dependency to `CMakeLists.txt`**

Insert this block directly after `FetchContent_MakeAvailable(nlohmann)`:

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

Replace the line `add_subdirectory(src/core)` with:

```cmake
add_subdirectory(src/core)
add_subdirectory(src/store)
```

In `tests/CMakeLists.txt`, replace `target_link_libraries(morpheus_tests PRIVATE morpheus_core Catch2::Catch2WithMain)` with:

```cmake
target_link_libraries(morpheus_tests PRIVATE morpheus_core morpheus_store Catch2::Catch2WithMain)
```

Create `src/store/CMakeLists.txt`:

```cmake
add_library(morpheus_store STATIC
    Sqlite.cpp
    RunStore.cpp
)
target_include_directories(morpheus_store PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_store
    PUBLIC morpheus_core
    PRIVATE sqlite3
)
morpheus_target_defaults(morpheus_store)
```

- [ ] **Step 2: Write the failing test `tests/store/RunStoreTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <stdexcept>
#include <string_view>

#include "TestPaths.h"
#include "core/Errors.h"
#include "store/RunStore.h"
#include "store/Sqlite.h"

using namespace morpheus::store;
using morpheus::core::ContractError;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Matches;

namespace {

RunMeta TestMeta()
{
    RunMeta meta;
    meta.worldId = "mini";
    meta.worldHash = "0123456789abcdef";
    meta.modelId = "test-model.gguf";
    meta.rngSeed = 18446744073709551615ULL; // Max value: proves the seed survives storage without sign issues.
    meta.settings = {{"n_ctx", 8192}};
    return meta;
}

nlohmann::json TestInitialState()
{
    return nlohmann::json::parse(R"({"player": {"type": "character", "fields": {"health": 10}}})");
}

std::filesystem::path FreshDbPath(std::string_view testName)
{
    return morpheus::test::FreshTempDir(testName) / "run.db";
}

} // namespace

TEST_CASE("RunStore::Create stores meta and the turn-0 snapshot", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_create");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
        CHECK(store.Meta().worldId == "mini");
        CHECK_THAT(store.Meta().createdAt, Matches(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"));
        CHECK(store.LatestTurn() == 0);
        CHECK(store.StateAt(0) == TestInitialState());
    }

    RunStore reopened = RunStore::Open(file);
    CHECK(reopened.Meta().worldHash == "0123456789abcdef");
    CHECK(reopened.Meta().modelId == "test-model.gguf");
    CHECK(reopened.Meta().rngSeed == 18446744073709551615ULL);
    CHECK(reopened.Meta().settings == nlohmann::json({{"n_ctx", 8192}}));
    CHECK(reopened.StateAt(0) == TestInitialState());
    CHECK_THROWS_AS(reopened.StateAt(1), ContractError);
}

TEST_CASE("RunStore refuses to overwrite a run or open the wrong file", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_refuse");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    CHECK_THROWS_AS(RunStore::Create(file, TestMeta(), TestInitialState()), StoreError);
    CHECK_THROWS_AS(RunStore::Open(file.parent_path() / "missing.db"), StoreError);

    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = '999' WHERE key = 'schema_version'");
    }
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("schema version"));
}

TEST_CASE("Database transactions roll back when the body throws", "[store]")
{
    Database db = Database::Open(FreshDbPath("store_transaction"), true);
    db.Exec("CREATE TABLE t (x INTEGER)");

    // The condition keeps the compiler from treating the commit path as unreachable code.
    bool fail = true;
    CHECK_THROWS_AS(db.Transaction([&] {
        db.Exec("INSERT INTO t VALUES (1)");
        if (fail) {
            throw std::runtime_error("boom");
        }
    }), std::runtime_error);

    Statement count = db.Prepare("SELECT COUNT(*) FROM t");
    REQUIRE(count.Step());
    CHECK(count.ColumnInt(0) == 0);
}
```

- [ ] **Step 3: Configure and build. Expect failure.**

Run: `cmake --preset tests`
Expected: the SQLite archive downloads and passes its SHA256 check, then configuration FAILS with `Cannot find source file: Sqlite.cpp` (from `src/store/CMakeLists.txt`).

- [ ] **Step 4: Implement `src/store/Sqlite.h`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace morpheus::store {

// Thrown when SQLite fails or a run database is missing, already exists, or has the wrong schema version.
class StoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One prepared statement. Bind parameters (1-based), then Step() through rows or Run() a statement without rows.
class Statement {
private:
    sqlite3* _pDb = nullptr;
    sqlite3_stmt* _pStmt = nullptr;

public:
    Statement(sqlite3* pDb, std::string_view sql);
    Statement(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& operator=(Statement&&) = delete;
    ~Statement();

    Statement& Bind(int index, std::int64_t value);
    Statement& Bind(int index, std::string_view text);
    // Stored as the raw bytes of the floats (little-endian on every supported platform). Must not be empty.
    Statement& Bind(int index, const std::vector<float>& values);

    // True when a row is available; false when the statement finished. Throws StoreError on failure.
    bool Step();

    // Executes a statement that must not return rows (INSERT, UPDATE, DELETE).
    void Run();

    std::int64_t ColumnInt(int index) const;
    std::string ColumnText(int index) const;
    std::vector<float> ColumnFloats(int index) const;

private:
    // Throws StoreError with SQLite's message unless rc is SQLITE_OK.
    void Check(int rc) const;
};

// Owns one SQLite connection.
class Database {
private:
    sqlite3* _pDb = nullptr;

public:
    // Opens a database file read-write; `create` allows creating it. Throws StoreError on failure.
    static Database Open(const std::filesystem::path& file, bool create);

    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    // Runs one or more SQL statements that return no rows.
    void Exec(std::string_view sql);

    Statement Prepare(std::string_view sql);

    // Runs body() between BEGIN IMMEDIATE and COMMIT. If body throws, rolls back and rethrows.
    template <class F>
    void Transaction(F&& body)
    {
        Exec("BEGIN IMMEDIATE");
        try {
            body();
        } catch (...) {
            RollbackNoThrow();
            throw;
        }
        Exec("COMMIT");
    }

private:
    explicit Database(sqlite3* pDb);

    // Used on the error path of Transaction, where a second exception would hide the first.
    void RollbackNoThrow() noexcept;
};

} // namespace morpheus::store
```

- [ ] **Step 5: Implement `src/store/Sqlite.cpp`**

```cpp
#include "store/Sqlite.h"

#include <cstring>
#include <format>
#include <utility>

#include <sqlite3.h>

#include "core/Errors.h"

namespace morpheus::store {

// ---- Statement ----------------------------------------------------------

Statement::Statement(sqlite3* pDb, std::string_view sql)
    : _pDb(pDb)
{
    MORPHEUS_REQUIRE(pDb != nullptr, "Statement: database is closed or moved-from");
    if (sqlite3_prepare_v2(pDb, sql.data(), static_cast<int>(sql.size()), &_pStmt, nullptr) != SQLITE_OK) {
        throw StoreError(std::format("cannot prepare SQL: {} | {}", sqlite3_errmsg(pDb), sql));
    }
}

Statement::Statement(Statement&& other) noexcept
    : _pDb(other._pDb)
    , _pStmt(std::exchange(other._pStmt, nullptr))
{
}

Statement::~Statement()
{
    sqlite3_finalize(_pStmt); // Harmless no-op on nullptr.
}

Statement& Statement::Bind(int index, std::int64_t value)
{
    Check(sqlite3_bind_int64(_pStmt, index, value));
    return *this;
}

Statement& Statement::Bind(int index, std::string_view text)
{
    // A default-constructed string_view has a null data pointer, which SQLite would store as NULL, not "".
    const char* pText = text.data() != nullptr ? text.data() : "";
    Check(sqlite3_bind_text(_pStmt, index, pText, static_cast<int>(text.size()), SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::Bind(int index, const std::vector<float>& values)
{
    MORPHEUS_REQUIRE(!values.empty(), "Statement::Bind: float blob must not be empty");
    Check(sqlite3_bind_blob(_pStmt, index, values.data(), static_cast<int>(values.size() * sizeof(float)),
                            SQLITE_TRANSIENT));
    return *this;
}

bool Statement::Step()
{
    const int rc = sqlite3_step(_pStmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw StoreError(std::format("SQL step failed: {}", sqlite3_errmsg(_pDb)));
}

void Statement::Run()
{
    const bool hasRow = Step();
    MORPHEUS_REQUIRE(!hasRow, "Statement::Run used on a statement that returns rows");
}

std::int64_t Statement::ColumnInt(int index) const
{
    return sqlite3_column_int64(_pStmt, index);
}

std::string Statement::ColumnText(int index) const
{
    const unsigned char* pText = sqlite3_column_text(_pStmt, index);
    const int size = sqlite3_column_bytes(_pStmt, index);
    if (pText == nullptr) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(pText), static_cast<std::size_t>(size));
}

std::vector<float> Statement::ColumnFloats(int index) const
{
    // SQLite requires reading the blob pointer before its size.
    const void* pBlob = sqlite3_column_blob(_pStmt, index);
    const int size = sqlite3_column_bytes(_pStmt, index);
    MORPHEUS_REQUIRE(size % static_cast<int>(sizeof(float)) == 0, "ColumnFloats: blob is not a whole number of floats");

    std::vector<float> values(static_cast<std::size_t>(size) / sizeof(float));
    if (size > 0) {
        std::memcpy(values.data(), pBlob, static_cast<std::size_t>(size));
    }
    return values;
}

void Statement::Check(int rc) const
{
    if (rc != SQLITE_OK) {
        throw StoreError(std::format("SQL bind failed: {}", sqlite3_errmsg(_pDb)));
    }
}

// ---- Database -----------------------------------------------------------

Database Database::Open(const std::filesystem::path& file, bool create)
{
    sqlite3* pDb = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    // SQLite expects UTF-8 file names; u8string keeps non-ASCII Windows paths intact.
    const std::u8string utf8Path = file.u8string();
    const int rc = sqlite3_open_v2(reinterpret_cast<const char*>(utf8Path.c_str()), &pDb, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = pDb != nullptr ? sqlite3_errmsg(pDb) : sqlite3_errstr(rc);
        sqlite3_close(pDb);
        throw StoreError(std::format("cannot open '{}': {}", file.generic_string(), message));
    }
    return Database(pDb);
}

Database::Database(sqlite3* pDb)
    : _pDb(pDb)
{
}

Database::Database(Database&& other) noexcept
    : _pDb(std::exchange(other._pDb, nullptr))
{
}

Database& Database::operator=(Database&& other) noexcept
{
    if (this != &other) {
        sqlite3_close(_pDb);
        _pDb = std::exchange(other._pDb, nullptr);
    }
    return *this;
}

Database::~Database()
{
    sqlite3_close(_pDb); // Harmless no-op on nullptr.
}

void Database::Exec(std::string_view sql)
{
    MORPHEUS_REQUIRE(_pDb != nullptr, "Database::Exec on a closed or moved-from database");
    const std::string text(sql); // sqlite3_exec needs a NUL-terminated string.
    char* pError = nullptr;
    if (sqlite3_exec(_pDb, text.c_str(), nullptr, nullptr, &pError) != SQLITE_OK) {
        const std::string message = pError != nullptr ? pError : "unknown error";
        sqlite3_free(pError);
        throw StoreError(std::format("SQL failed: {} | {}", message, text));
    }
}

Statement Database::Prepare(std::string_view sql)
{
    return Statement(_pDb, sql);
}

void Database::RollbackNoThrow() noexcept
{
    sqlite3_exec(_pDb, "ROLLBACK", nullptr, nullptr, nullptr);
}

} // namespace morpheus::store
```

- [ ] **Step 6: Implement `src/store/RunStore.h`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "store/Sqlite.h"

namespace morpheus::store {

struct RunMeta {
    std::string worldId = {};
    std::string worldHash = {}; // World content hash at run creation.
    std::string modelId = {};
    std::uint64_t rngSeed = 0;
    nlohmann::json settings = nlohmann::json::object();
    std::string createdAt = {}; // ISO-8601 UTC, set by RunStore::Create.
};

// One run's SQLite database (saves/<world>/<run>.db). The only writer of run data.
// Tables: run_meta, turns, state_snapshots, summaries, notes, embeddings.
class RunStore {
public:
    static constexpr int SCHEMA_VERSION = 1;

private:
    Database _db;
    RunMeta _meta = {};

public:
    // Creates a new run database holding the meta and the turn-0 snapshot. Throws StoreError if the file exists.
    static RunStore Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState);

    // Opens an existing run. Throws StoreError if it is missing or was written with another schema version.
    static RunStore Open(const std::filesystem::path& file);

    const RunMeta& Meta() const;

    // Highest committed turn number, 0 before the first turn.
    int LatestTurn();

    // State after `turn` (0 = initial state). Throws ContractError if there is no snapshot for it.
    nlohmann::json StateAt(int turn);

private:
    RunStore(Database db, RunMeta meta);
};

} // namespace morpheus::store
```

- [ ] **Step 7: Implement `src/store/RunStore.cpp`**

```cpp
#include "store/RunStore.h"

#include <array>
#include <chrono>
#include <format>
#include <map>
#include <utility>

#include "core/Errors.h"

namespace morpheus::store {

namespace {

constexpr const char* SCHEMA_SQL = R"sql(
CREATE TABLE run_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE turns (
    number    INTEGER PRIMARY KEY,
    input     TEXT NOT NULL,
    resolve   TEXT NOT NULL,
    rolls     TEXT NOT NULL,
    applied   TEXT NOT NULL,
    rejected  TEXT NOT NULL,
    narration TEXT NOT NULL,
    timings   TEXT NOT NULL
);
CREATE TABLE state_snapshots (
    turn  INTEGER PRIMARY KEY,
    state TEXT NOT NULL
);
CREATE TABLE summaries (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    from_turn INTEGER NOT NULL,
    to_turn   INTEGER NOT NULL,
    text      TEXT NOT NULL
);
CREATE TABLE notes (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    turn INTEGER NOT NULL,
    text TEXT NOT NULL
);
CREATE TABLE embeddings (
    kind   TEXT NOT NULL,
    ref_id TEXT NOT NULL,
    model  TEXT NOT NULL,
    vector BLOB NOT NULL,
    PRIMARY KEY (kind, ref_id, model)
);
)sql";

} // namespace

RunStore RunStore::Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState)
{
    if (std::filesystem::exists(file)) {
        throw StoreError(std::format("run database '{}' already exists", file.generic_string()));
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    meta.createdAt = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                 std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    // Schema, meta and the turn-0 snapshot land together or not at all.
    Database db = Database::Open(file, true);
    db.Transaction([&] {
        db.Exec(SCHEMA_SQL);

        const std::array<std::pair<const char*, std::string>, 7> metaRows = {{
            {"schema_version", std::to_string(SCHEMA_VERSION)},
            {"world_id", meta.worldId},
            {"world_hash", meta.worldHash},
            {"model_id", meta.modelId},
            {"rng_seed", std::to_string(meta.rngSeed)}, // Text: SQLite integers are signed 64-bit.
            {"settings", meta.settings.dump()},
            {"created_at", meta.createdAt},
        }};
        for (const auto& [key, value] : metaRows) {
            db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
        }

        db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (0, ?1)").Bind(1, initialState.dump()).Run();
    });
    return RunStore(std::move(db), std::move(meta));
}

RunStore RunStore::Open(const std::filesystem::path& file)
{
    if (!std::filesystem::exists(file)) {
        throw StoreError(std::format("run database '{}' does not exist", file.generic_string()));
    }
    Database db = Database::Open(file, false);

    // Read the key/value meta table in one pass.
    std::map<std::string, std::string> values;
    {
        Statement query = db.Prepare("SELECT key, value FROM run_meta");
        while (query.Step()) {
            values.emplace(query.ColumnText(0), query.ColumnText(1));
        }
    }
    auto value = [&](const char* key) -> const std::string& {
        const auto it = values.find(key);
        if (it == values.end()) {
            throw StoreError(std::format("'{}': run_meta has no '{}'", file.generic_string(), key));
        }
        return it->second;
    };

    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     file.generic_string(), value("schema_version"), SCHEMA_VERSION));
    }

    RunMeta meta;
    meta.worldId = value("world_id");
    meta.worldHash = value("world_hash");
    meta.modelId = value("model_id");
    meta.rngSeed = std::stoull(value("rng_seed"));
    meta.settings = nlohmann::json::parse(value("settings"));
    meta.createdAt = value("created_at");
    return RunStore(std::move(db), std::move(meta));
}

const RunMeta& RunStore::Meta() const
{
    return _meta;
}

int RunStore::LatestTurn()
{
    Statement query = _db.Prepare("SELECT COALESCE(MAX(number), 0) FROM turns");
    const bool hasRow = query.Step();
    MORPHEUS_REQUIRE(hasRow, "LatestTurn: aggregate query returned no row");
    return static_cast<int>(query.ColumnInt(0));
}

nlohmann::json RunStore::StateAt(int turn)
{
    Statement query = _db.Prepare("SELECT state FROM state_snapshots WHERE turn = ?1");
    query.Bind(1, turn);
    const bool found = query.Step();
    MORPHEUS_REQUIRE(found, std::format("StateAt: no snapshot for turn {}", turn));
    return nlohmann::json::parse(query.ColumnText(0));
}

RunStore::RunStore(Database db, RunMeta meta)
    : _db(std::move(db))
    , _meta(std::move(meta))
{
}

} // namespace morpheus::store
```

- [ ] **Step 8: Build and run the tests. Expect them to pass.**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including the 3 `[store]` tests.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt src/store tests/store
git commit -m "Add SQLite wrapper and run database creation" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Turns, snapshots and undo

**Files:**
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `RunStore`, `Database`, `Statement` (Task 8).
- Produces (namespace `morpheus::store`):
  - `struct TurnRecord { int number; std::string input; nlohmann::json resolve, rolls, applied, rejected; std::string narration; nlohmann::json timings; }`
  - `void RunStore::CommitTurn(const TurnRecord&, const nlohmann::json& stateAfter)`
  - `std::optional<TurnRecord> RunStore::Turn(int)`
  - `std::vector<TurnRecord> RunStore::Turns(int fromTurn, int toTurn)`
  - `void RunStore::TruncateAfter(int turn)`

- [ ] **Step 1: Append the failing tests to `tests/store/RunStoreTests.cpp`**

Add `#include <format>` to the includes at the top of the file. Then append:

```cpp
namespace {

TurnRecord TestTurn(int number)
{
    TurnRecord record;
    record.number = number;
    record.input = std::format("input {}", number);
    record.resolve = {{"intent", "climb"}};
    record.rolls = nlohmann::json::array({nlohmann::json{{"total", 14}}});
    record.applied = nlohmann::json::array({nlohmann::json{{"op", "set"}}});
    record.rejected = nlohmann::json::array();
    record.narration = std::format("narration {}", number);
    record.timings = {{"resolve_ms", 120}};
    return record;
}

nlohmann::json StateWithHealth(int health)
{
    return {{"player", {{"type", "character"}, {"fields", {{"health", health}}}}}};
}

} // namespace

TEST_CASE("RunStore commits turns with their resulting state", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_turns"), TestMeta(), TestInitialState());
    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    store.CommitTurn(TestTurn(2), StateWithHealth(8));

    CHECK(store.LatestTurn() == 2);
    CHECK(store.StateAt(1) == StateWithHealth(9));
    CHECK(store.StateAt(2) == StateWithHealth(8));

    const std::optional<TurnRecord> first = store.Turn(1);
    REQUIRE(first.has_value());
    CHECK(first->input == "input 1");
    CHECK(first->resolve == nlohmann::json({{"intent", "climb"}}));
    CHECK(first->rolls.at(0).at("total") == 14);
    CHECK(first->applied.size() == 1);
    CHECK(first->rejected.empty());
    CHECK(first->narration == "narration 1");
    CHECK(first->timings.at("resolve_ms") == 120);

    const std::vector<TurnRecord> both = store.Turns(1, 2);
    REQUIRE(both.size() == 2);
    CHECK(both[0].number == 1);
    CHECK(both[1].number == 2);
    CHECK_FALSE(store.Turn(3).has_value());
}

TEST_CASE("RunStore only accepts the next turn number", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_turn_order"), TestMeta(), TestInitialState());
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(2), StateWithHealth(9)), ContractError);
    CHECK(store.LatestTurn() == 0);

    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(1), StateWithHealth(9)), ContractError);
    CHECK(store.LatestTurn() == 1);
}

TEST_CASE("TruncateAfter undoes later turns and snapshots", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_truncate"), TestMeta(), TestInitialState());
    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    store.CommitTurn(TestTurn(2), StateWithHealth(8));
    store.CommitTurn(TestTurn(3), StateWithHealth(7));

    store.TruncateAfter(1);
    CHECK(store.LatestTurn() == 1);
    CHECK_FALSE(store.Turn(2).has_value());
    CHECK_THROWS_AS(store.StateAt(2), ContractError);
    CHECK(store.StateAt(1) == StateWithHealth(9));

    // The run continues from turn 1 as if turns 2 and 3 never happened.
    store.CommitTurn(TestTurn(2), StateWithHealth(5));
    CHECK(store.StateAt(2) == StateWithHealth(5));
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C2065: 'TurnRecord': undeclared identifier` (or a similar unknown-identifier error).

- [ ] **Step 3: Extend `src/store/RunStore.h`**

Add `#include <optional>` and `#include <vector>` to the includes. Add this struct directly after `struct RunMeta { ... };`:

```cpp
// Everything one committed turn produced, as stored in the turns table. JSON members hold the turn module's
// structured outputs verbatim.
struct TurnRecord {
    int number = 0;
    std::string input = {};
    nlohmann::json resolve = nlohmann::json::object(); // Resolve call output.
    nlohmann::json rolls = nlohmann::json::array();    // Check results.
    nlohmann::json applied = nlohmann::json::array();  // Applied changes.
    nlohmann::json rejected = nlohmann::json::array(); // Rejected changes with reasons.
    std::string narration = {};
    nlohmann::json timings = nlohmann::json::object();
};
```

Add these public methods directly after `nlohmann::json StateAt(int turn);`:

```cpp
    // Appends turn LatestTurn()+1 and the state it produced, in one transaction.
    // Throws ContractError if record.number is not LatestTurn()+1.
    void CommitTurn(const TurnRecord& record, const nlohmann::json& stateAfter);

    // nullopt when the turn does not exist.
    std::optional<TurnRecord> Turn(int number);

    // Turns in [fromTurn, toTurn], ascending.
    std::vector<TurnRecord> Turns(int fromTurn, int toTurn);

    // Undo: removes every turn, snapshot and note after `turn`, every turn embedding whose turn is after it,
    // and every summary that ends after it. Throws ContractError if turn < 0.
    void TruncateAfter(int turn);
```

- [ ] **Step 4: Implement the new methods in `src/store/RunStore.cpp`**

Insert these directly after the `RunStore::StateAt` definition:

```cpp
void RunStore::CommitTurn(const TurnRecord& record, const nlohmann::json& stateAfter)
{
    const int expected = LatestTurn() + 1;
    MORPHEUS_REQUIRE(record.number == expected,
                     std::format("CommitTurn: expected turn {}, got {}", expected, record.number));

    // The turn and its resulting state land together or not at all.
    _db.Transaction([&] {
        _db.Prepare("INSERT INTO turns (number, input, resolve, rolls, applied, rejected, narration, timings) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)")
            .Bind(1, record.number)
            .Bind(2, record.input)
            .Bind(3, record.resolve.dump())
            .Bind(4, record.rolls.dump())
            .Bind(5, record.applied.dump())
            .Bind(6, record.rejected.dump())
            .Bind(7, record.narration)
            .Bind(8, record.timings.dump())
            .Run();
        _db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (?1, ?2)")
            .Bind(1, record.number)
            .Bind(2, stateAfter.dump())
            .Run();
    });
}

std::optional<TurnRecord> RunStore::Turn(int number)
{
    std::vector<TurnRecord> found = Turns(number, number);
    if (found.empty()) {
        return std::nullopt;
    }
    return std::move(found.front());
}

std::vector<TurnRecord> RunStore::Turns(int fromTurn, int toTurn)
{
    Statement query = _db.Prepare("SELECT number, input, resolve, rolls, applied, rejected, narration, timings "
                                  "FROM turns WHERE number BETWEEN ?1 AND ?2 ORDER BY number");
    query.Bind(1, fromTurn).Bind(2, toTurn);

    std::vector<TurnRecord> records;
    while (query.Step()) {
        TurnRecord record;
        record.number = static_cast<int>(query.ColumnInt(0));
        record.input = query.ColumnText(1);
        record.resolve = nlohmann::json::parse(query.ColumnText(2));
        record.rolls = nlohmann::json::parse(query.ColumnText(3));
        record.applied = nlohmann::json::parse(query.ColumnText(4));
        record.rejected = nlohmann::json::parse(query.ColumnText(5));
        record.narration = query.ColumnText(6);
        record.timings = nlohmann::json::parse(query.ColumnText(7));
        records.push_back(std::move(record));
    }
    return records;
}

void RunStore::TruncateAfter(int turn)
{
    MORPHEUS_REQUIRE(turn >= 0, "TruncateAfter: turn must be >= 0");

    // Everything that happened after `turn` goes, in one transaction. Summaries that end at or before `turn`
    // only cover unchanged turns, so they stay.
    _db.Transaction([&] {
        _db.Prepare("DELETE FROM turns WHERE number > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM state_snapshots WHERE turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM summaries WHERE to_turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM notes WHERE turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM embeddings WHERE kind = 'turn' AND CAST(ref_id AS INTEGER) > ?1").Bind(1, turn).Run();
    });
}
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/store tests/store
git commit -m "Add turn commits, snapshots and undo to RunStore" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Summaries, notes and embeddings

**Files:**
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `RunStore`, `TurnRecord`, `TruncateAfter` (Task 9).
- Produces (namespace `morpheus::store`):
  - `struct Summary { int fromTurn; int toTurn; std::string text; }`
  - `struct Note { int turn; std::string text; }`
  - `enum class EmbeddingKind { Lore, Turn }`
  - `struct EmbeddingRow { EmbeddingKind kind; std::string refId; std::string model; std::vector<float> vector; }`
  - `void RunStore::AddSummary(const Summary&)`, `std::vector<Summary> RunStore::Summaries()`
  - `void RunStore::AddNote(const Note&)`, `std::vector<Note> RunStore::Notes()`
  - `void RunStore::PutEmbedding(const EmbeddingRow&)`, `std::vector<EmbeddingRow> RunStore::Embeddings(EmbeddingKind, const std::string& model)`
- Convention: a turn embedding's `refId` is the turn number in decimal ("12"). A lore embedding's `refId` is the lore subject.

- [ ] **Step 1: Append the failing tests to `tests/store/RunStoreTests.cpp`**

Add `#include <set>` to the includes at the top of the file. Then append:

```cpp
TEST_CASE("RunStore keeps summaries and notes in order", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_summaries"), TestMeta(), TestInitialState());
    store.AddSummary({5, 8, "second fold"});
    store.AddSummary({1, 4, "first fold"});
    store.AddNote({3, "the captain distrusts Marta"});
    store.AddNote({1, "storm season"});

    const std::vector<Summary> summaries = store.Summaries();
    REQUIRE(summaries.size() == 2);
    CHECK(summaries[0].text == "first fold");
    CHECK(summaries[1].fromTurn == 5);
    CHECK(summaries[1].toTurn == 8);

    const std::vector<Note> notes = store.Notes();
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].turn == 1);
    CHECK(notes[1].text == "the captain distrusts Marta");

    CHECK_THROWS_AS(store.AddSummary({4, 2, "backwards"}), ContractError);
}

TEST_CASE("RunStore stores embeddings per kind and model", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_embeddings"), TestMeta(), TestInitialState());
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {0.25f, -1.5f, 3.0f}});
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {1.0f}}); // Replaces the previous vector.
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-b", {2.0f}});
    store.PutEmbedding({EmbeddingKind::Turn, "1", "embed-a", {0.5f, -0.125f}});

    const std::vector<EmbeddingRow> lore = store.Embeddings(EmbeddingKind::Lore, "embed-a");
    REQUIRE(lore.size() == 1);
    CHECK(lore[0].refId == "boarding");
    CHECK(lore[0].vector == std::vector<float>{1.0f});

    const std::vector<EmbeddingRow> turns = store.Embeddings(EmbeddingKind::Turn, "embed-a");
    REQUIRE(turns.size() == 1);
    CHECK(turns[0].kind == EmbeddingKind::Turn);
    CHECK(turns[0].vector == std::vector<float>{0.5f, -0.125f});

    CHECK_THROWS_AS(store.PutEmbedding({EmbeddingKind::Turn, "2", "embed-a", {}}), ContractError);
}

TEST_CASE("TruncateAfter also undoes later summaries, notes and turn embeddings", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_truncate_memory"), TestMeta(), TestInitialState());
    for (int turn = 1; turn <= 5; ++turn) {
        store.CommitTurn(TestTurn(turn), StateWithHealth(10 - turn));
    }
    store.AddSummary({1, 3, "kept"});
    store.AddSummary({4, 5, "dropped"});
    store.AddNote({3, "kept"});
    store.AddNote({4, "dropped"});
    store.PutEmbedding({EmbeddingKind::Turn, "3", "embed-a", {1.0f}});
    store.PutEmbedding({EmbeddingKind::Turn, "4", "embed-a", {1.0f}});
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {1.0f}});

    store.TruncateAfter(3);

    REQUIRE(store.Summaries().size() == 1);
    CHECK(store.Summaries()[0].text == "kept");
    REQUIRE(store.Notes().size() == 1);
    CHECK(store.Notes()[0].text == "kept");

    std::set<std::string> turnRefs;
    for (const EmbeddingRow& row : store.Embeddings(EmbeddingKind::Turn, "embed-a")) {
        turnRefs.insert(row.refId);
    }
    CHECK(turnRefs == std::set<std::string>{"3"});
    CHECK(store.Embeddings(EmbeddingKind::Lore, "embed-a").size() == 1);
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `'Summary': undeclared identifier` (or a similar unknown-identifier error).

- [ ] **Step 3: Extend `src/store/RunStore.h`**

Add these types directly after `struct TurnRecord { ... };`:

```cpp
// A folded range of turns (story summary, layer L2).
struct Summary {
    int fromTurn = 0;
    int toTurn = 0;
    std::string text = {};
};

// An append-only memory written by the model in assisted memory mode.
struct Note {
    int turn = 0;
    std::string text = {};
};

enum class EmbeddingKind { Lore, Turn };

// refId: lore subject for Lore, decimal turn number for Turn.
struct EmbeddingRow {
    EmbeddingKind kind = EmbeddingKind::Lore;
    std::string refId = {};
    std::string model = {};
    std::vector<float> vector = {};
};
```

Add these public methods directly after `void TruncateAfter(int turn);`:

```cpp
    // Throws ContractError unless 1 <= fromTurn <= toTurn.
    void AddSummary(const Summary& summary);

    // Ordered by fromTurn.
    std::vector<Summary> Summaries();

    void AddNote(const Note& note);

    // Ordered by turn, then insertion.
    std::vector<Note> Notes();

    // Inserts or replaces the vector for (kind, refId, model). Throws ContractError on an empty vector.
    void PutEmbedding(const EmbeddingRow& row);

    // Every embedding of one kind produced by one embedding model.
    std::vector<EmbeddingRow> Embeddings(EmbeddingKind kind, const std::string& model);
```

- [ ] **Step 4: Implement the new methods in `src/store/RunStore.cpp`**

Inside the existing anonymous namespace, after `SCHEMA_SQL`, add:

```cpp
// Stored spelling of an embedding kind. TruncateAfter's SQL relies on "turn".
const char* KindName(EmbeddingKind kind)
{
    switch (kind) {
    case EmbeddingKind::Lore:
        return "lore";
    case EmbeddingKind::Turn:
        return "turn";
    }
    throw morpheus::core::ContractError("KindName: unhandled EmbeddingKind");
}
```

Insert these directly after the `RunStore::TruncateAfter` definition:

```cpp
void RunStore::AddSummary(const Summary& summary)
{
    MORPHEUS_REQUIRE(summary.fromTurn >= 1 && summary.fromTurn <= summary.toTurn,
                     std::format("AddSummary: bad turn range {}..{}", summary.fromTurn, summary.toTurn));
    _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
        .Bind(1, summary.fromTurn)
        .Bind(2, summary.toTurn)
        .Bind(3, summary.text)
        .Run();
}

std::vector<Summary> RunStore::Summaries()
{
    Statement query = _db.Prepare("SELECT from_turn, to_turn, text FROM summaries ORDER BY from_turn, id");
    std::vector<Summary> summaries;
    while (query.Step()) {
        summaries.push_back({static_cast<int>(query.ColumnInt(0)), static_cast<int>(query.ColumnInt(1)),
                             query.ColumnText(2)});
    }
    return summaries;
}

void RunStore::AddNote(const Note& note)
{
    _db.Prepare("INSERT INTO notes (turn, text) VALUES (?1, ?2)").Bind(1, note.turn).Bind(2, note.text).Run();
}

std::vector<Note> RunStore::Notes()
{
    Statement query = _db.Prepare("SELECT turn, text FROM notes ORDER BY turn, id");
    std::vector<Note> notes;
    while (query.Step()) {
        notes.push_back({static_cast<int>(query.ColumnInt(0)), query.ColumnText(1)});
    }
    return notes;
}

void RunStore::PutEmbedding(const EmbeddingRow& row)
{
    MORPHEUS_REQUIRE(!row.vector.empty(), "PutEmbedding: vector must not be empty");
    _db.Prepare("INSERT OR REPLACE INTO embeddings (kind, ref_id, model, vector) VALUES (?1, ?2, ?3, ?4)")
        .Bind(1, KindName(row.kind))
        .Bind(2, row.refId)
        .Bind(3, row.model)
        .Bind(4, row.vector)
        .Run();
}

std::vector<EmbeddingRow> RunStore::Embeddings(EmbeddingKind kind, const std::string& model)
{
    Statement query = _db.Prepare("SELECT ref_id, vector FROM embeddings WHERE kind = ?1 AND model = ?2 ORDER BY ref_id");
    query.Bind(1, KindName(kind)).Bind(2, model);

    std::vector<EmbeddingRow> rows;
    while (query.Step()) {
        rows.push_back({kind, query.ColumnText(0), model, query.ColumnFloats(1)});
    }
    return rows;
}
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/store tests/store
git commit -m "Add summaries, notes and embeddings to RunStore" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Milestone verification

**Files:**
- No new files. Fix anything these steps uncover in the file that caused it, and commit the fix.

**Interfaces:**
- Consumes: everything above.
- Produces: confirmation that the milestone builds and passes under AddressSanitizer, and that the prototype app still builds with the new CMake layout.

- [ ] **Step 1: Run the full suite under AddressSanitizer**

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `100% tests passed`, with no `AddressSanitizer` report in the output.

- [ ] **Step 2: Confirm the app configuration still builds**

Run: `cmake --preset dev && cmake --build --preset dev`
Expected: the configure step fetches llama.cpp, SDL3 and ImGui, and the build produces `build/dev/bin/Debug/morpheus.exe` plus `morpheus_tests.exe`. This takes a while on the first run. Do not change `src/main.cpp` to fix warnings: that target is not built with `/WX`, and milestone 4 replaces it.

- [ ] **Step 3: Run the tests from the dev build**

Run: `ctest --preset dev`
Expected: `100% tests passed`.

- [ ] **Step 4: Commit fixes, if any**

If Steps 1-3 required changes, commit the changed files:

```bash
git add <changed files>
git commit -m "Fix issues found in milestone 1 verification" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

If nothing changed, there is nothing to commit.
