### Task 3: The catalog of worlds and runs

The Runs window lists the world folders and each world's saved runs, suggests a name for a new run, and reads an optional seed. This task also creates the `src/app` module with its first library.

**Files:**
- Create: `src/app/CMakeLists.txt`, `src/app/model/Catalog.h`, `src/app/model/Catalog.cpp`, `tests/app/CatalogTests.cpp`
- Modify: `CMakeLists.txt` (add `src/app`), `tests/CMakeLists.txt` (link `morpheus_app_model`)

**Interfaces:**
- Consumes: `core::LoadWorld`, `core::World::{id, title}`, `core::PathText`, `core::Trim`, `core::DataError`.
- Produces (namespace `morpheus::app`):
  - The constant `RUN_NAME_MAX_CHARS = 64`.
  - `struct WorldEntry { std::filesystem::path folder; std::string id; std::string title; std::string error; }`.
  - `struct RunEntry { std::filesystem::path file; std::string name; std::filesystem::file_time_type lastWrite; }`.
  - `struct SeedInput { std::optional<std::uint64_t> seed; std::string problem; }`.
  - `std::vector<WorldEntry> ListWorlds(const std::filesystem::path& projectsDir)`.
  - `std::vector<RunEntry> ListRuns(const std::filesystem::path& savesDir, std::string_view worldId)`.
  - `std::filesystem::path RunFile(const std::filesystem::path& savesDir, std::string_view worldId, std::string_view name)`.
  - `std::string RunNameProblem(const std::filesystem::path& savesDir, std::string_view worldId, std::string_view name)`.
  - `std::string DefaultRunName(std::chrono::system_clock::time_point now)`.
  - `SeedInput ParseSeed(std::string_view text)`.
  - `std::string LocalTimeText(std::filesystem::file_time_type time)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/app/CatalogTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "TestPaths.h"
#include "app/model/Catalog.h"
#include "core/Errors.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Matches;

TEST_CASE("ListWorlds lists world folders with their titles, and broken worlds with their error", "[catalog]")
{
    const std::filesystem::path projects = test::FreshTempDir("catalog_worlds");
    std::filesystem::copy(test::TestDataDir() / "worlds" / "mini", projects / "mini",
                          std::filesystem::copy_options::recursive);
    std::filesystem::create_directories(projects / "broken");
    test::WriteFile(projects / "broken" / "world.json", "{}");
    std::filesystem::create_directories(projects / "notes"); // No world.json: not a world.
    test::WriteFile(projects / "readme.txt", "not a world");

    const std::vector<app::WorldEntry> worlds = app::ListWorlds(projects);
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0].id == "broken"); // The folder name, since the world does not load.
    CHECK_FALSE(worlds[0].error.empty());
    CHECK(worlds[1].id == "mini");
    CHECK(worlds[1].title == "Mini World");
    CHECK(worlds[1].error.empty());
    CHECK(worlds[1].folder == projects / "mini");

    CHECK_THROWS_AS(app::ListWorlds(projects / "missing"), core::DataError);
}

TEST_CASE("ListRuns lists a world's runs, most recently played first", "[catalog]")
{
    const std::filesystem::path saves = test::FreshTempDir("catalog_runs");
    CHECK(app::ListRuns(saves, "mini").empty()); // No folder yet.

    std::filesystem::create_directories(saves / "mini");
    test::WriteFile(saves / "mini" / "old.db", "");
    test::WriteFile(saves / "mini" / "new.db", "");
    test::WriteFile(saves / "mini" / "new.db-wal", ""); // SQLite's side files are not runs.
    test::WriteFile(saves / "mini" / "notes.txt", "");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(saves / "mini" / "old.db", now - std::chrono::hours(2));
    std::filesystem::last_write_time(saves / "mini" / "new.db", now - std::chrono::hours(1));

    const std::vector<app::RunEntry> runs = app::ListRuns(saves, "mini");
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].name == "new");
    CHECK(runs[1].name == "old");
    CHECK(runs[0].file == app::RunFile(saves, "mini", "new"));
    CHECK_THAT(app::LocalTimeText(runs[0].lastWrite), Matches(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2})"));
}

TEST_CASE("RunNameProblem accepts plain new names only", "[catalog]")
{
    const std::filesystem::path saves = test::FreshTempDir("catalog_names");
    CHECK(app::RunNameProblem(saves, "mini", "first-run_2").empty());
    CHECK(app::RunNameProblem(saves, "mini", std::string(app::RUN_NAME_MAX_CHARS, 'a')).empty());
    CHECK_FALSE(app::RunNameProblem(saves, "mini", "").empty());
    CHECK_FALSE(app::RunNameProblem(saves, "mini", std::string(app::RUN_NAME_MAX_CHARS + 1, 'a')).empty());
    CHECK_FALSE(app::RunNameProblem(saves, "mini", "my run").empty());
    CHECK_FALSE(app::RunNameProblem(saves, "mini", "../escape").empty());

    std::filesystem::create_directories(saves / "mini");
    test::WriteFile(app::RunFile(saves, "mini", "taken"), "");
    CHECK_THAT(app::RunNameProblem(saves, "mini", "taken"), ContainsSubstring("already exists"));
}

TEST_CASE("DefaultRunName stamps the UTC time to the second", "[catalog]")
{
    const auto when = std::chrono::sys_days(std::chrono::year(2026) / 10 / 28) + std::chrono::hours(14) +
                      std::chrono::minutes(5) + std::chrono::seconds(9) + std::chrono::milliseconds(700);
    CHECK(app::DefaultRunName(when) == "run-20261028-140509");
}

TEST_CASE("ParseSeed reads blank as random and accepts only a whole number in range", "[catalog]")
{
    CHECK_FALSE(app::ParseSeed("").seed);
    CHECK(app::ParseSeed("  ").problem.empty());
    CHECK(app::ParseSeed(" 42 ").seed == std::optional<std::uint64_t>(42));
    CHECK(app::ParseSeed("18446744073709551615").seed == std::optional<std::uint64_t>(18446744073709551615ull));
    CHECK_FALSE(app::ParseSeed("18446744073709551616").problem.empty());
    CHECK_FALSE(app::ParseSeed("-1").problem.empty());
    CHECK_FALSE(app::ParseSeed("12abc").problem.empty());
}
```

- [ ] **Step 2: Create the module and run the tests to verify they fail**

Create `src/app/CMakeLists.txt`:

```cmake
# The app's state and actions, without SDL, ImGui or llama.cpp: every preset builds and tests it.
add_library(morpheus_app_model STATIC
    model/Catalog.cpp
)
target_include_directories(morpheus_app_model PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_app_model PUBLIC morpheus_turn)
morpheus_target_defaults(morpheus_app_model)
```

In the root `CMakeLists.txt`, after the `# ---- Engine libraries` block's `endif()` (the one around `src/llama`, `tools/probe` and `tools/play`), add:

```cmake
# ---- App ---------------------------------------------------------------
# The model and UI libraries build with the tests too (ImGui is fetched for both); the executable needs the app.
if(MORPHEUS_BUILD_APP OR MORPHEUS_BUILD_TESTS)
    add_subdirectory(src/app)
endif()
```

In `tests/CMakeLists.txt`, add `morpheus_app_model` to the `target_link_libraries(morpheus_tests PRIVATE ...)` line.

Run: `cmake --preset tests && cmake --build --preset tests`
Expected: compile errors: `app/model/Catalog.h` is missing (and so is `model/Catalog.cpp`).

- [ ] **Step 3: Write the header**

Create `src/app/model/Catalog.h`:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace morpheus::app {

constexpr std::size_t RUN_NAME_MAX_CHARS = 64; // A run's name is also its file name.

// A world folder under game_projects/.
struct WorldEntry {
    std::filesystem::path folder = {};
    std::string id = {};    // world.json's id, or the folder name when the world does not load.
    std::string title = {};
    std::string error = {}; // Why the world does not load; empty when it does.
};

// A saved run: saves/<world id>/<name>.db.
struct RunEntry {
    std::filesystem::path file = {};
    std::string name = {};
    std::filesystem::file_time_type lastWrite = {}; // When it was last played.
};

// A run seed as the player typed it.
struct SeedInput {
    std::optional<std::uint64_t> seed = {}; // nullopt: pick one at random.
    std::string problem = {};               // Why the text is not a seed; empty when it is one (or blank).
};

// Every subfolder of `projectsDir` holding a world.json, by folder name. A world that does not load (DataError) is
// listed with its error, so the player sees why it cannot be played. Throws DataError when projectsDir is not a
// folder.
std::vector<WorldEntry> ListWorlds(const std::filesystem::path& projectsDir);

// The runs of world `worldId` (savesDir/worldId/*.db), most recently played first. Empty when there are none.
std::vector<RunEntry> ListRuns(const std::filesystem::path& savesDir, std::string_view worldId);

// The file of run `name` of world `worldId`: savesDir/worldId/name.db.
std::filesystem::path RunFile(const std::filesystem::path& savesDir, std::string_view worldId, std::string_view name);

// Why `name` cannot name a new run of world `worldId`, or "" when it can. A name has 1..RUN_NAME_MAX_CHARS
// letters, digits, '-' or '_', and no run of that name may exist yet.
std::string RunNameProblem(const std::filesystem::path& savesDir, std::string_view worldId, std::string_view name);

// The name suggested for a new run: "run-YYYYMMDD-HHMMSS", in UTC.
std::string DefaultRunName(std::chrono::system_clock::time_point now);

// Reads a typed seed: blank (after trimming) means random; otherwise a decimal integer in 0..2^64-1.
SeedInput ParseSeed(std::string_view text);

// A file time as local "YYYY-MM-DD HH:MM", for the runs list.
std::string LocalTimeText(std::filesystem::file_time_type time);

} // namespace morpheus::app
```

- [ ] **Step 4: Write the implementation**

Create `src/app/model/Catalog.cpp`:

```cpp
#include "app/model/Catalog.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "core/World.h"

namespace morpheus::app {

std::vector<WorldEntry> ListWorlds(const std::filesystem::path& projectsDir)
{
    if (!std::filesystem::is_directory(projectsDir)) {
        throw core::DataError(std::format("worlds folder '{}' does not exist", core::PathText(projectsDir)));
    }
    std::vector<WorldEntry> worlds;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(projectsDir)) {
        // Only folders with a world.json are worlds; anything else in the folder is ignored.
        if (!entry.is_directory() || !std::filesystem::exists(entry.path() / "world.json")) {
            continue;
        }
        WorldEntry world;
        world.folder = entry.path();
        try {
            const core::World loaded = core::LoadWorld(entry.path());
            world.id = loaded.id;
            world.title = loaded.title;
        } catch (const core::DataError& error) {
            // Listed with its error rather than hidden: the player sees what to fix.
            world.id = core::PathText(entry.path().filename());
            world.error = error.what();
        }
        worlds.push_back(std::move(world));
    }
    std::sort(worlds.begin(), worlds.end(),
              [](const WorldEntry& a, const WorldEntry& b) { return a.folder.filename() < b.folder.filename(); });
    return worlds;
}

std::vector<RunEntry> ListRuns(const std::filesystem::path& savesDir, std::string_view worldId)
{
    std::vector<RunEntry> runs;
    const std::filesystem::path folder = savesDir / std::string(worldId);
    if (!std::filesystem::is_directory(folder)) {
        return runs; // No run of this world yet.
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder)) {
        // A run is one .db file; SQLite's side files (.db-wal, .db-shm) have other extensions.
        if (entry.is_regular_file() && entry.path().extension() == ".db") {
            runs.push_back({entry.path(), core::PathText(entry.path().stem()), entry.last_write_time()});
        }
    }
    std::sort(runs.begin(), runs.end(), [](const RunEntry& a, const RunEntry& b) {
        return a.lastWrite != b.lastWrite ? a.lastWrite > b.lastWrite : a.name < b.name;
    });
    return runs;
}

std::filesystem::path RunFile(const std::filesystem::path& savesDir, std::string_view worldId, std::string_view name)
{
    return savesDir / std::string(worldId) / (std::string(name) + ".db");
}

std::string RunNameProblem(const std::filesystem::path& savesDir, std::string_view worldId, std::string_view name)
{
    if (name.empty() || name.size() > RUN_NAME_MAX_CHARS) {
        return std::format("a run name needs 1 to {} characters", RUN_NAME_MAX_CHARS);
    }
    // Only characters that are safe in a file name on every system, so the name is the file name as typed.
    const bool plain = std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
    if (!plain) {
        return "a run name may only use letters, digits, '-' and '_'";
    }
    if (std::filesystem::exists(RunFile(savesDir, worldId, name))) {
        return std::format("a run named '{}' already exists", name);
    }
    return {};
}

std::string DefaultRunName(std::chrono::system_clock::time_point now)
{
    return std::format("run-{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(now));
}

SeedInput ParseSeed(std::string_view text)
{
    const std::string trimmed = core::Trim(text);
    if (trimmed.empty()) {
        return {}; // Blank: a random seed.
    }
    // The whole text must be decimal digits that fit 64 bits; from_chars rejects signs for unsigned types.
    std::uint64_t value = 0;
    const char* pEnd = trimmed.data() + trimmed.size();
    const auto [pNext, error] = std::from_chars(trimmed.data(), pEnd, value);
    if (error != std::errc() || pNext != pEnd) {
        return {std::nullopt, std::format("'{}' is not a seed: use a whole number from 0 to {}", trimmed,
                                          std::numeric_limits<std::uint64_t>::max())};
    }
    return {value, {}};
}

std::string LocalTimeText(std::filesystem::file_time_type time)
{
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
    const std::chrono::zoned_time local(std::chrono::current_zone(), std::chrono::floor<std::chrono::minutes>(system));
    return std::format("{:%Y-%m-%d %H:%M}", local);
}

} // namespace morpheus::app
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[catalog]"`
Expected: all five tests pass. Then `ctest --preset tests`: everything passes.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt src/app/CMakeLists.txt src/app/model/Catalog.h src/app/model/Catalog.cpp tests/app/CatalogTests.cpp
git commit -m "List worlds and runs for the app" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

