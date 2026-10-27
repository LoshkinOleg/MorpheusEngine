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
