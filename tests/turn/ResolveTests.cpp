#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/World.h"
#include "fakes/FakeBackend.h"
#include "turn/EngineText.h"
#include "turn/Resolve.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

constexpr const char* CLIMB = R"({"intent":"climb","checks":[{"stat":"agility","difficulty":"hard","stakes":"a fall",)"
                              R"("on_success":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs"}],)"
                              R"("on_failure":[{"op":"add","path":"player.health","value":-2,"reason":"falls"}]}],)"
                              R"("changes":[{"op":"set","path":"player.location","value":"cliff base","reason":"walks over"}]})";

} // namespace

TEST_CASE("ParseResolveOutput reads a proposal and reports unreadable output", "[resolve]")
{
    const turn::ResolveProposal proposal =
        turn::ParseResolveOutput(R"({"intent":"listen","checks":[],"changes":[],"notes":["a"]})");
    CHECK(proposal.error.empty());
    CHECK(proposal.intent == "listen");
    CHECK(proposal.changes == nlohmann::json::array());
    CHECK(proposal.notes == std::vector<std::string>{"a"});

    CHECK_THAT(turn::ParseResolveOutput(R"({"intent":"listen","checks":[)").error, ContainsSubstring("not complete JSON"));
    CHECK_THAT(turn::ParseResolveOutput(R"({"checks":[],"changes":[]})").error, ContainsSubstring("'intent'"));
    CHECK_THAT(turn::ParseResolveOutput(R"({"intent":"x","checks":[{"stat":1}],"changes":[]})").error,
               ContainsSubstring("check 1"));
    const turn::ResolveProposal bad = turn::ParseResolveOutput(R"({"intent":"x","checks":{},"changes":[]})");
    CHECK(bad.intent.empty()); // Nothing is kept from unreadable output.
}

TEST_CASE("ApplyProposal rolls checks and applies the matching branch after the unconditional changes", "[resolve]")
{
    const core::World world = MiniWorld();
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(CLIMB);
    REQUIRE(proposal.error.empty());
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    const core::CheckResult expected = core::RollCheck(world.dice, 3, 16, core::CheckSeed(42, 1, 0));
    REQUIRE(resolution.checks.size() == 1);
    CHECK(resolution.checks[0].stat == "agility");
    CHECK(resolution.checks[0].stakes == "a fall");
    CHECK(resolution.checks[0].result.rolls == expected.rolls);
    CHECK(resolution.checks[0].result.success == expected.success);

    const nlohmann::json& fields = resolution.state.at("player").at("fields");
    CHECK(fields.at("location") == (expected.success ? "cliff top" : "cliff base"));
    CHECK(fields.at("health") == (expected.success ? 10 : 8));
    REQUIRE(resolution.applied.size() == 2);
    CHECK(resolution.applied[0].change.reason == "walks over"); // Unconditional changes come first.

    // Same seed, same turn: the same rolls (regenerate keeps the dice).
    CHECK(turn::ApplyProposal(world, world.initialState, proposal, 42, 1).checks[0].result.rolls == expected.rolls);
}

TEST_CASE("A check with an unknown stat is rejected and keeps later checks' seeds", "[resolve]")
{
    const core::World world = MiniWorld();
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"x","checks":[)"
        R"({"stat":"strength","difficulty":"easy","stakes":"","on_success":[{"op":"set","path":"player.location","value":"x","reason":""}],"on_failure":[]},)"
        R"({"stat":"agility","difficulty":"easy","stakes":"","on_success":[],"on_failure":[]}],"changes":[]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    REQUIRE(resolution.checks.size() == 1);
    CHECK(resolution.checks[0].result.rolls == core::RollCheck(world.dice, 3, 8, core::CheckSeed(42, 1, 1)).rolls);
    REQUIRE(resolution.rejected.size() == 1);
    CHECK_THAT(resolution.rejected[0].reason, ContainsSubstring("unknown stat 'strength'"));
    CHECK(resolution.state.at("player").at("fields").at("location") == "shore");
}

TEST_CASE("RenderNarrateMessage reports the resolution, then the narrate instruction", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"climb","checks":[],"changes":[{"op":"add","path":"player.health","value":-2,"reason":"scrapes"},)"
        R"({"op":"add","path":"player.inventory.oil","value":-1,"reason":"spills"}]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    const std::string message = turn::RenderNarrateMessage(proposal, resolution, 512, backend);
    CHECK(message == "# Resolution\nIntent: climb\nApplied: player.health -2\n"
                     "Rejected: add player.inventory.oil (no item 'oil' to decrease)\n\n# Task\n" +
                         std::string(turn::NARRATE_INSTRUCTION));

    // Over the allowance, the rejected changes go first.
    const std::string squeezed = turn::RenderNarrateMessage(proposal, resolution, backend.CountTokens(message) - 1, backend);
    CHECK_THAT(squeezed, !ContainsSubstring("Rejected:"));
    CHECK_THAT(squeezed, ContainsSubstring("Applied: player.health -2"));
    CHECK_THROWS_AS(turn::RenderNarrateMessage(proposal, resolution, 5, backend), core::DataError);
}

TEST_CASE("RenderNarrateMessage shows each roll and an unreadable proposal", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"hop","checks":[{"stat":"agility","difficulty":"easy","stakes":"a slip","on_success":[],"on_failure":[]}],"changes":[]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);
    const core::CheckResult roll = core::RollCheck(world.dice, 3, 8, core::CheckSeed(42, 1, 0));
    CHECK_THAT(turn::RenderNarrateMessage(proposal, resolution, 512, backend),
               ContainsSubstring(std::format("Check: agility vs easy (8): rolled {} +3 = {}, {}. Stakes: a slip\n"
                                             "Applied: none\n",
                                             roll.rolls.at(0), roll.total, roll.success ? "success" : "failure")));

    const turn::ResolveProposal unreadable = turn::ParseResolveOutput(R"({"intent":"hop")");
    const turn::Resolution nothing = turn::ApplyProposal(world, world.initialState, unreadable, 42, 1);
    CHECK(nothing.applied.empty());
    CHECK(nothing.state == world.initialState);
    CHECK_THAT(turn::RenderNarrateMessage(unreadable, nothing, 512, backend),
               ContainsSubstring("The action could not be resolved"));
}

TEST_CASE("ParseLookupOutput reads lookups and reports unreadable output", "[resolve]")
{
    std::string error;
    const std::vector<turn::Lookup> lookups = turn::ParseLookupOutput(
        R"({"lookups":[{"source":"lore","query":"keeper"},{"source":"history","query":"the key"},{"source":"notes","query":"debts"}]})",
        error);
    CHECK(error.empty());
    REQUIRE(lookups.size() == 3);
    CHECK(lookups[0].source == store::EmbeddingKind::Lore);
    CHECK(lookups[1].source == store::EmbeddingKind::Turn);
    CHECK(lookups[2].source == store::EmbeddingKind::Note);
    CHECK(lookups[1].query == "the key");

    CHECK(turn::ParseLookupOutput(R"({"lookups":[{"source":"dreams","query":"x"}]})", error).empty());
    CHECK_THAT(error, ContainsSubstring("source"));
}
