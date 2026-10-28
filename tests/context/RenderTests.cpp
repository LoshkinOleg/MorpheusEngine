#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "context/StateView.h"
#include "context/Transcript.h"
#include "context/WorldReference.h"
#include "core/World.h"

using namespace morpheus;

namespace {

core::TurnRecord TurnWithMechanics()
{
    core::TurnRecord record;
    record.number = 3;
    record.input = "I climb the hull.";
    record.narration = "You slip and hit the deck.";
    core::CheckOutcome check;
    check.stat = "agility";
    check.difficulty = "hard";
    check.result.total = 14;
    check.result.target = 16;
    check.result.success = false;
    record.checks = {check};
    record.applied = {
        {core::Change{core::ChangeOp::Set, "player.location", "deck", ""}, ""},
        {core::Change{core::ChangeOp::Add, "player.health", -3, ""}, "clamped to 0"},
        {core::Change{core::ChangeOp::Add, "player.conditions", "bruised", ""}, ""},
        {core::Change{core::ChangeOp::Remove, "player.inventory.rope", nullptr, ""}, ""},
        {core::Change{core::ChangeOp::Create, "npc:marta", nlohmann::json{{"type", "npc"}}, ""}, ""},
    };
    record.rejected = {{nlohmann::json{{"op", "explode"}}, "unknown op"}};
    return record;
}

} // namespace

TEST_CASE("RenderStateView prints one compact line per entity", "[context]")
{
    const nlohmann::json state = nlohmann::json::parse(R"({
        "player": {"type": "captain", "fields": {"health": 10, "location": "open desert", "conditions": [],
                                                  "inventory": {"pistol": 1, "compass": 1}}},
        "crawler": {"type": "crawler", "fields": {"crew": 20, "vehicles": {"sand_bike": 2}, "morale": "steady"}}
    })");
    CHECK(context::RenderStateView(state) ==
          "crawler (crawler): crew 20, morale \"steady\", vehicles {sand_bike 2}\n"
          "player (captain): conditions [], health 10, inventory {compass 1, pistol 1}, location \"open desert\"\n");
}

TEST_CASE("RenderMechanicsLine summarizes checks and applied changes", "[context]")
{
    CHECK(context::RenderMechanicsLine(TurnWithMechanics()) ==
          "[agility vs hard: 14/16 failure; player.location = \"deck\"; player.health -3 (clamped to 0); "
          "player.conditions +bruised; player.inventory.rope removed; npc:marta created]");

    core::TurnRecord quiet;
    quiet.input = "I wait.";
    CHECK(context::RenderMechanicsLine(quiet).empty());
}

TEST_CASE("RenderTranscript turns each turn into a user and an assistant message", "[context]")
{
    core::TurnRecord quiet;
    quiet.input = "I wait.";
    quiet.narration = "Nothing happens.";

    const std::vector<inference::Message> messages = context::RenderTranscript({TurnWithMechanics(), quiet});
    REQUIRE(messages.size() == 4);
    CHECK(messages[0].role == inference::Role::User);
    CHECK(messages[0].content == "I climb the hull.\n" + context::RenderMechanicsLine(TurnWithMechanics()));
    CHECK(messages[1].role == inference::Role::Assistant);
    CHECK(messages[1].content == "You slip and hit the deck.");
    CHECK(messages[2].content == "I wait."); // No mechanics: no extra line.
    CHECK(messages[3].content == "Nothing happens.");
}

TEST_CASE("RenderWorldReference lists checks, stats and entity types", "[context]")
{
    const core::World world = core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
    CHECK(context::RenderWorldReference(world) ==
          "# Game reference\n"
          "Checks roll 1d20+stat and succeed at or above the difficulty: easy 8, hard 16.\n"
          "Stats (int fields of player): agility.\n"
          "Entity types:\n"
          "- character: agility (int 0..5), health (int 0..10), inventory (counter), location (string)\n"
          "- npc (creatable): disposition (enum: wary, friendly)\n");
}

TEST_CASE("RenderMechanicsLine covers every change form", "[context]")
{
    core::TurnRecord record;
    core::CheckResult success;
    success.total = 18;
    success.target = 8;
    success.success = true;
    record.checks = {{"grit", "easy", "", success}};
    record.applied = {
        {core::Change{core::ChangeOp::Add, "player.inventory.water", 2, ""}, ""},
        {core::Change{core::ChangeOp::Add, "player.inventory.rope", -1, ""}, "none left"},
        {core::Change{core::ChangeOp::Remove, "player.conditions", "tired", ""}, ""},
    };
    CHECK(context::RenderMechanicsLine(record) ==
          "[grit vs easy: 18/8 success; player.inventory.water +2; player.inventory.rope -1 (none left); "
          "player.conditions -tired]");
    CHECK(context::RenderChangePart(record.applied[1]) == "player.inventory.rope -1 (none left)");
}

TEST_CASE("RenderStateView prints list items and an empty state", "[context]")
{
    CHECK(context::RenderStateView(nlohmann::json::object()).empty());
    const nlohmann::json state = nlohmann::json::parse(
        R"({"player": {"type": "captain", "fields": {"conditions": ["tired", "hungry"]}}})");
    CHECK(context::RenderStateView(state) == "player (captain): conditions [tired, hungry]\n");
}
