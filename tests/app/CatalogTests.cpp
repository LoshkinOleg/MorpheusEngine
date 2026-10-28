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
