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
