#include "app/model/Catalog.h"

#include <algorithm>
#include <cctype>
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
    // Windows reserves these device names for every extension, so "con.db" cannot be created there. Compared without
    // case, as the file system does.
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
    const bool deviceNumber = lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) &&
                              lower[3] >= '1' && lower[3] <= '9';
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul" || deviceNumber) {
        return std::format("'{}' is a reserved device name on Windows", name);
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
