#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/World.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

} // namespace

TEST_CASE("The resolve grammar names the world's stats, difficulties and field shapes", "[grammar]")
{
    const core::World world = MiniWorld();
    const std::string grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);

    CHECK_THAT(grammar, ContainsSubstring(R"(root ::= "{\"intent\":" short-text ",\"checks\":" checks ",\"changes\":" changes "}")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(stat ::= "\"agility\"")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(difficulty ::= "\"easy\"" | "\"hard\"")"));
    // An int field: set or add an integer.
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":" ("\"set\"" | "\"add\"") ",\"path\":\"player.health\",\"value\":" int reason)"));
    // A string field: set only.
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"set\"" ",\"path\":\"player.location\",\"value\":" short-text reason)"));
    // A counter field: add a non-zero count to an item, or remove the item.
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"add\",\"path\":\"player.inventory." item "\",\"value\":" nonzero-int)"));
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"remove\",\"path\":\"player.inventory." item "\"")"));
    // Create: a new id and every field of the creatable type.
    CHECK_THAT(grammar, ContainsSubstring(
        R"(",\"value\":{\"type\":\"npc\",\"fields\":{" "\"disposition\":" enum-0 "}}" reason)"));
    CHECK_THAT(grammar, ContainsSubstring(R"(enum-0 ::= "\"wary\"" | "\"friendly\"")"));
    CHECK_THAT(grammar, !ContainsSubstring("notes"));
}

TEST_CASE("The resolve grammar follows the live state and the memory mode", "[grammar]")
{
    const core::World world = MiniWorld();
    nlohmann::json state = world.initialState;
    state["npc:marta"] = {{"type", "npc"}, {"fields", {{"disposition", "wary"}}}};

    const std::string grammar = turn::BuildResolveGrammar(world, state, core::MemoryMode::Assisted);
    CHECK_THAT(grammar, ContainsSubstring(R"(\"npc:marta.disposition\")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(",\"notes\":" notes "}")"));
    CHECK_THAT(grammar, ContainsSubstring("notes ::= "));
}

TEST_CASE("The lookup grammar lists sources and queries", "[grammar]")
{
    const std::string grammar = turn::BuildLookupGrammar();
    CHECK_THAT(grammar, ContainsSubstring(R"(root ::= "{\"lookups\":" lookups "}")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(("\"lore\"" | "\"history\"" | "\"notes\""))"));
}
