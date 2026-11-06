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

