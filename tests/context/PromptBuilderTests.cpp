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
    return {64, 64, 48, 512, 256, 512, 256};
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

// Template-inclusive quantities of an assembly, measured on a backend with room to spare, so tests can pick
// exact context sizes from them.
struct Measured {
    int systemCost = 0;     // The system message alone, templated.
    int transcriptCost = 0; // What L3 adds to the templated prompt.
    int tail = 0;           // Raw tail tokens.
};

Measured Measure(const core::World& world, const context::ContextInputs& inputs, const core::BudgetSettings& budget)
{
    test::FakeBackend roomy(100000);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, budget, roomy);
    return {roomy.CountTokens(assembly.prompt.messages[0].content) + 4, assembly.transcriptCost, assembly.tokens.tail};
}

// The context size that gives L3 exactly `transcriptBudget` tokens.
int ContextSizeFor(const core::BudgetSettings& budget, const Measured& measured, int transcriptBudget)
{
    return budget.resolveMaxTokens + budget.narrateMaxTokens + budget.narrateAllowance + budget.tailAllowance +
           measured.systemCost + transcriptBudget;
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

TEST_CASE("AssembleResolvePrompt budgets the transcript with template-inclusive costs", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, Inputs(world), RoomyBudget(), backend);

    const int systemCost = backend.CountTokens(assembly.prompt.messages[0].content) + 4;
    CHECK(assembly.transcriptBudget == 8192 - 64 - 64 - 48 - 512 - systemCost);
    CHECK(assembly.transcriptCost ==
          backend.CountTokens("I knock.") + 4 + backend.CountTokens("The door creaks.") + 4);
    CHECK_FALSE(assembly.transcriptOverBudget);
}

TEST_CASE("AssembleResolvePrompt reports budget overruns", "[context]")
{
    const core::World world = MiniWorld();
    const core::BudgetSettings roomy = RoomyBudget();

    SECTION("transcript exactly at its budget fits")
    {
        const context::ContextInputs inputs = Inputs(world);
        const Measured measured = Measure(world, inputs, roomy);
        test::FakeBackend backend(ContextSizeFor(roomy, measured, measured.transcriptCost));
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, roomy, backend);
        CHECK(assembly.transcriptBudget == measured.transcriptCost);
        CHECK_FALSE(assembly.transcriptOverBudget);
        CHECK_FALSE(assembly.summaryOverBudget);
        CHECK_FALSE(assembly.tailOverBudget);
        CHECK_FALSE(assembly.contextOverflow);
        CHECK(assembly.Fits());
    }
    SECTION("transcript one token over its budget")
    {
        context::ContextInputs inputs = Inputs(world);
        inputs.transcript.front().narration = std::string(400, 'x');
        const Measured measured = Measure(world, inputs, roomy);
        test::FakeBackend backend(ContextSizeFor(roomy, measured, measured.transcriptCost - 1));
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, roomy, backend);
        CHECK(assembly.transcriptBudget == measured.transcriptCost - 1);
        CHECK(assembly.transcriptOverBudget);
        CHECK_FALSE(assembly.summaryOverBudget);
        CHECK_FALSE(assembly.tailOverBudget);
        CHECK_FALSE(assembly.contextOverflow);
        CHECK_FALSE(assembly.Fits());
    }
    SECTION("only the context overflows")
    {
        // The tail exactly fills its allowance, so the 4 template tokens of its message push the prompt over.
        const context::ContextInputs inputs = Inputs(world);
        core::BudgetSettings budget = roomy;
        budget.tailAllowance = Measure(world, inputs, roomy).tail;
        const Measured measured = Measure(world, inputs, budget);
        test::FakeBackend backend(ContextSizeFor(budget, measured, measured.transcriptCost));
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, budget, backend);
        CHECK(assembly.contextOverflow);
        CHECK_FALSE(assembly.transcriptOverBudget);
        CHECK_FALSE(assembly.summaryOverBudget);
        CHECK_FALSE(assembly.tailOverBudget);
        CHECK_FALSE(assembly.Fits());
    }
    SECTION("tail over its allowance")
    {
        test::FakeBackend backend(8192);
        context::ContextInputs inputs = Inputs(world);
        inputs.playerInput = std::string(4000, 'y');
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, roomy, backend);
        CHECK(assembly.tailOverBudget);
        CHECK_FALSE(assembly.transcriptOverBudget);
        CHECK_FALSE(assembly.summaryOverBudget);
        CHECK_FALSE(assembly.contextOverflow);
    }
    SECTION("summary over its cap")
    {
        test::FakeBackend backend(8192);
        context::ContextInputs inputs = Inputs(world);
        inputs.summaries.front().text = std::string(2000, 's');
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, roomy, backend);
        CHECK(assembly.summaryOverBudget);
        CHECK_FALSE(assembly.transcriptOverBudget);
        CHECK_FALSE(assembly.tailOverBudget);
        CHECK_FALSE(assembly.contextOverflow);
    }
    SECTION("a context too small for the fixed layers is a data error, not a flag")
    {
        const context::ContextInputs inputs = Inputs(world);
        const Measured measured = Measure(world, inputs, roomy);
        test::FakeBackend small(ContextSizeFor(roomy, measured, -10));
        CHECK_THROWS_AS(context::AssembleResolvePrompt(world, inputs, roomy, small), core::DataError);
        CHECK_THROWS_WITH(context::AssembleResolvePrompt(world, inputs, roomy, small),
                          ContainsSubstring("no room for the transcript"));
        test::FakeBackend zero(ContextSizeFor(roomy, measured, 0)); // Nothing left is no room either.
        CHECK_THROWS_WITH(context::AssembleResolvePrompt(world, inputs, roomy, zero),
                          ContainsSubstring("no room for the transcript"));
        test::FakeBackend one(ContextSizeFor(roomy, measured, 1));
        CHECK_NOTHROW(context::AssembleResolvePrompt(world, inputs, roomy, one));
    }
    SECTION("world over its cap is a data error")
    {
        test::FakeBackend backend(8192);
        core::BudgetSettings budget = roomy;
        budget.worldCap = 2;
        CHECK_THROWS_WITH(context::AssembleResolvePrompt(world, Inputs(world), budget, backend), ContainsSubstring("world_cap"));
    }
    SECTION("engine rules over their cap are an engine bug")
    {
        test::FakeBackend backend(8192);
        core::BudgetSettings budget = roomy;
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

TEST_CASE("The narrate call fits the context whenever the resolve prompt fits", "[context]")
{
    const core::World world = MiniWorld();
    const context::ContextInputs inputs = Inputs(world);

    // Worst case: the tail uses its whole allowance, the resolve output all of resolve_max_tokens and the
    // resolution record all of narrate_allowance (one fake token per 4 bytes).
    core::BudgetSettings budget = RoomyBudget();
    budget.tailAllowance = Measure(world, inputs, budget).tail;
    const Measured measured = Measure(world, inputs, budget);
    const std::string resolveOutput(static_cast<std::size_t>(budget.resolveMaxTokens) * 4, 'r');
    const std::string resolution(static_cast<std::size_t>(budget.narrateAllowance) * 4, 'n');

    // The context the narrate call needs, measured on a backend with room to spare.
    test::FakeBackend roomy(100000);
    const context::Assembly roomyAssembly = context::AssembleResolvePrompt(world, inputs, budget, roomy);
    REQUIRE(roomy.CountTokens(resolveOutput) == budget.resolveMaxTokens);
    REQUIRE(roomy.CountTokens(resolution) == budget.narrateAllowance);
    inference::Prompt roomyNarrate = roomyAssembly.prompt;
    context::ExtendForNarrate(roomyNarrate, resolveOutput, resolution);
    const int needed = roomy.CountPromptTokens(roomyNarrate) + budget.narrateMaxTokens;

    // At exactly that context the resolve prompt fits, and the worst-case narrate call fits with no token to spare.
    {
        test::FakeBackend backend(needed);
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, budget, backend);
        REQUIRE(assembly.transcriptBudget >= measured.transcriptCost);
        REQUIRE(assembly.Fits());
        inference::Prompt narrate = assembly.prompt;
        context::ExtendForNarrate(narrate, resolveOutput, resolution);
        CHECK(backend.CountPromptTokens(narrate) + budget.narrateMaxTokens <= backend.ContextSize());
    }

    // Any smaller context cannot hold the worst case, so the resolve prompt must not report that it fits. The
    // transcript still has room there: the overflow flag alone catches it.
    for (int missing = 1; missing <= 4; ++missing) {
        test::FakeBackend backend(needed - missing);
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, budget, backend);
        CHECK_FALSE(assembly.transcriptOverBudget);
        CHECK(assembly.contextOverflow);
        CHECK_FALSE(assembly.Fits());
    }
}

TEST_CASE("narrate_allowance and the narrate message overhead count toward the context overflow", "[context]")
{
    const core::World world = MiniWorld();
    const core::BudgetSettings budget = RoomyBudget();
    const context::ContextInputs inputs = Inputs(world);
    const Measured measured = Measure(world, inputs, budget);

    // The tail leaves no slack: total (which holds the tail message's 4 template tokens) + both generations +
    // allowance + 4 template tokens each for the assistant and the user message of the narrate call is exactly the
    // context at tailAllowance = tail + 4 + 4 + 4, and one token over below that.
    core::BudgetSettings exact = budget;
    exact.tailAllowance = measured.tail + 4 + 4 + 4;
    {
        const Measured exactMeasured = Measure(world, inputs, exact);
        test::FakeBackend backend(ContextSizeFor(exact, exactMeasured, exactMeasured.transcriptCost));
        CHECK_FALSE(context::AssembleResolvePrompt(world, inputs, exact, backend).contextOverflow);
    }
    core::BudgetSettings tight = exact;
    tight.tailAllowance -= 1;
    {
        const Measured tightMeasured = Measure(world, inputs, tight);
        test::FakeBackend backend(ContextSizeFor(tight, tightMeasured, tightMeasured.transcriptCost));
        CHECK(context::AssembleResolvePrompt(world, inputs, tight, backend).contextOverflow);
    }
}

TEST_CASE("The world reference sits in L1 between the rules and the lore", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(100000);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, Inputs(world), RoomyBudget(), backend);
    const std::string& system = assembly.prompt.messages.front().content;
    const std::size_t rules = system.find("You are the GM of a tiny test world.");
    const std::size_t reference = system.find("# Game reference");
    const std::size_t lore = system.find("# Lore");
    REQUIRE(reference != std::string::npos);
    CHECK(rules < reference);
    CHECK(reference < lore);
}

TEST_CASE("An over-cap summary is a flag even when it leaves the transcript no room", "[context]")
{
    const core::World world = MiniWorld();
    core::BudgetSettings budget = RoomyBudget(); // summary_cap 256
    context::ContextInputs inputs = Inputs(world);
    inputs.summaries = {{1, 4, std::string(4000, 's')}}; // About 1000 tokens.
    // The tail exactly fills its allowance, so the context has no slack for the template tokens and overflows.
    budget.tailAllowance = Measure(world, inputs, budget).tail;
    const Measured measured = Measure(world, inputs, budget);
    // One token short of room for the full system message: before this change, a no-room DataError.
    test::FakeBackend backend(budget.resolveMaxTokens + budget.narrateMaxTokens + budget.narrateAllowance +
                              budget.tailAllowance + measured.systemCost - 1);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, budget, backend);
    CHECK(assembly.summaryOverBudget);
    CHECK(assembly.contextOverflow);
    CHECK(assembly.transcriptBudget > 0);
}
