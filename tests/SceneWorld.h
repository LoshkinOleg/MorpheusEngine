#pragma once

#include <filesystem>
#include <string_view>

#include "TestPaths.h"

namespace morpheus::test {

// A copy of the mini test world in FreshTempDir(name)/mini (the folder name is the world id) that declares a scene,
// the player's location, and has lore for two places: "shore", where the player starts, and "cliff top". The rest of
// the lore is the mini world's. Returns the world folder.
inline std::filesystem::path SceneWorldDir(std::string_view name)
{
    const std::filesystem::path dir = FreshTempDir(name) / "mini";
    std::filesystem::copy(TestDataDir() / "worlds" / "mini", dir, std::filesystem::copy_options::recursive);
    WriteFile(dir / "world.json",
              R"({"id": "mini", "title": "Mini World", "dice": "1d20+stat", "check_entity": "player",)"
              R"( "stats": ["agility"], "difficulties": {"easy": 8, "hard": 16}, "memory_mode": "engine",)"
              R"( "scene": {"paths": ["player.location"], "present_at": "player.location"}})");
    WriteFile(dir / "lore.csv", "subject,data,always_on\n"
                                "lighthouse,\"A tall lighthouse, painted red.\",true\n"
                                "keeper,\"The keeper says \"\"hello\"\" to everyone.\",false\n"
                                "storm,Storms arrive at dusk.,\n"
                                "shore,Grey pebbles and driftwood line the shore.,\n"
                                "cliff top,Wind howls over the bare cliff top.,\n");
    return dir;
}

} // namespace morpheus::test
