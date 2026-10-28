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
