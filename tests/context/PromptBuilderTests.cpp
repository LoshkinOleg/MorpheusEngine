#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestPaths.h"
#include "context/PromptBuilder.h"
#include "core/Errors.h"
#include "core/World.h"
#include "fakes/FakeBackend.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

core::BudgetSettings RoomyBudget()
{
    return {64, 64, 512, 256, 512, 256};
}

context::ContextInputs Inputs(const core::World& world)
{
    context::ContextInputs inputs;
    inputs.engineRules = "ENGINE RULES";
    inputs.summaries = {{1, 4, "The keeper lit the lamp."}};
    core::TurnRecord turn;
    turn.number = 5;
    turn.input = "I knock.";
    turn.narration = "The door creaks.";
    inputs.transcript = {turn};
    inputs.state = world.initialState;
    inputs.retrieved = {"storm: Storms arrive at dusk."};
    inputs.playerInput = "I go inside.";
    inputs.instruction = "RESOLVE INSTRUCTION";
    return inputs;
}

} // namespace

TEST_CASE("AssembleResolvePrompt lays out the layers", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, Inputs(world), RoomyBudget(), backend);
    const inference::Prompt& prompt = assembly.prompt;

    REQUIRE(prompt.messages.size() == 4);
    CHECK(prompt.stableCount == 3);

    const std::string& system = prompt.messages[0].content;
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(system.starts_with("ENGINE RULES\n\n# World\n"));
    CHECK_THAT(system, ContainsSubstring("tiny test world"));
    CHECK_THAT(system, ContainsSubstring("# Lore\n- lighthouse: A tall lighthouse, painted red.\n"));
    CHECK_THAT(system, !ContainsSubstring("keeper:")); // Not always-on.
    CHECK_THAT(system, ContainsSubstring("# Story so far\nTurns 1-4: The keeper lit the lamp.\n"));

    CHECK(prompt.messages[1].content == "I knock.");
    CHECK(prompt.messages[2].content == "The door creaks.");

    const std::string& tail = prompt.messages[3].content;
    CHECK(prompt.messages[3].role == inference::Role::User);
    CHECK(tail.starts_with("# Current state\nplayer (character):"));
    CHECK_THAT(tail, ContainsSubstring("\n# Relevant\n- storm: Storms arrive at dusk.\n"));
    CHECK_THAT(tail, ContainsSubstring("\n# Player\nI go inside.\n\n# Task\nRESOLVE INSTRUCTION"));
}

TEST_CASE("AssembleResolvePrompt leaves out empty optional layers", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    context::ContextInputs inputs = Inputs(world);
    inputs.summaries.clear();
    inputs.retrieved.clear();
    inputs.transcript.clear();

    const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
    REQUIRE(assembly.prompt.messages.size() == 2);
    CHECK(assembly.prompt.stableCount == 1);
    CHECK_THAT(assembly.prompt.messages[0].content, !ContainsSubstring("# Story so far"));
    CHECK_THAT(assembly.prompt.messages[1].content, !ContainsSubstring("# Relevant"));
}

TEST_CASE("AssembleResolvePrompt counts tokens per layer", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    const context::ContextInputs inputs = Inputs(world);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);

    CHECK(assembly.tokens.engineRules == backend.CountTokens("ENGINE RULES"));
    CHECK(assembly.tokens.transcript == backend.CountTokens("I knock.") + backend.CountTokens("The door creaks."));
    CHECK(assembly.tokens.tail == backend.CountTokens(assembly.prompt.messages[3].content));
    CHECK(assembly.tokens.total == backend.CountPromptTokens(assembly.prompt));
    CHECK(assembly.Fits());
}

TEST_CASE("AssembleResolvePrompt reports budget overruns", "[context]")
{
    const core::World world = MiniWorld();

    SECTION("transcript over its budget")
    {
        test::FakeBackend tight(64 + 64 + 512 + 60); // Room for the fixed layers, almost none for L3.
        context::ContextInputs inputs = Inputs(world);
        inputs.transcript.front().narration = std::string(400, 'x');
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), tight);
        CHECK(assembly.transcriptOverBudget);
        CHECK_FALSE(assembly.Fits());
    }
    SECTION("tail over its allowance")
    {
        test::FakeBackend backend(8192);
        context::ContextInputs inputs = Inputs(world);
        inputs.playerInput = std::string(4000, 'y');
        CHECK(context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend).tailOverBudget);
    }
    SECTION("summary over its cap")
    {
        test::FakeBackend backend(8192);
        context::ContextInputs inputs = Inputs(world);
        inputs.summaries.front().text = std::string(2000, 's');
        CHECK(context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend).summaryOverBudget);
    }
    SECTION("world over its cap is a data error")
    {
        test::FakeBackend backend(8192);
        core::BudgetSettings budget = RoomyBudget();
        budget.worldCap = 2;
        CHECK_THROWS_WITH(context::AssembleResolvePrompt(world, Inputs(world), budget, backend), ContainsSubstring("world_cap"));
    }
    SECTION("engine rules over their cap are an engine bug")
    {
        test::FakeBackend backend(8192);
        core::BudgetSettings budget = RoomyBudget();
        budget.engineRulesCap = 1;
        CHECK_THROWS_AS(context::AssembleResolvePrompt(world, Inputs(world), budget, backend), core::ContractError);
    }
}

TEST_CASE("ExtendForNarrate only appends", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, Inputs(world), RoomyBudget(), backend);

    inference::Prompt narrate = assembly.prompt;
    context::ExtendForNarrate(narrate, "{\"intent\":\"enter\"}", "RESOLUTION + NARRATE INSTRUCTION");

    REQUIRE(narrate.messages.size() == assembly.prompt.messages.size() + 2);
    CHECK(narrate.stableCount == assembly.prompt.stableCount);
    for (std::size_t i = 0; i < assembly.prompt.messages.size(); ++i) {
        CHECK(narrate.messages[i].content == assembly.prompt.messages[i].content);
    }
    CHECK(narrate.messages[4].role == inference::Role::Assistant);
    CHECK(narrate.messages[4].content == "{\"intent\":\"enter\"}");
    CHECK(narrate.messages[5].role == inference::Role::User);
    CHECK(narrate.messages[5].content == "RESOLUTION + NARRATE INSTRUCTION");
}
