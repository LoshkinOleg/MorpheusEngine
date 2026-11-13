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

