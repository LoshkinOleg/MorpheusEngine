### Task 8: Prompt assembly and budgets

**Files:**
- Create: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`, `tests/context/PromptBuilderTests.cpp`
- Modify: `src/context/CMakeLists.txt` (add `PromptBuilder.cpp`)

**Interfaces:**
- Consumes:
  - `RenderStateView`, `RenderTranscript` (Task 7)
  - `core::World`, `core::LoreEntry`, `core::Summary`, `core::TurnRecord`, `core::BudgetSettings`
  - `inference::LlmBackend`
  - `morpheus::test::FakeBackend` (tests)
- Produces (namespace `morpheus::context`):
  - `struct ContextInputs { std::string engineRules; std::vector<core::Summary> summaries; std::vector<core::TurnRecord> transcript; nlohmann::json state; std::vector<std::string> retrieved; std::string playerInput; std::string instruction; }`
  - `struct LayerTokens { int engineRules, world, summary, transcript, tail, total; }`
  - `struct Assembly`, holding:
    - `inference::Prompt prompt`, `LayerTokens tokens`, `int transcriptBudget`
    - the flags `transcriptOverBudget`, `summaryOverBudget`, `tailOverBudget` and `contextOverflow`
    - `bool Fits() const`
  - `Assembly AssembleResolvePrompt(const core::World&, const ContextInputs&, const core::BudgetSettings&, inference::LlmBackend&)`
  - `void ExtendForNarrate(inference::Prompt&, const std::string& resolveOutput, const std::string& resolutionAndInstruction)`
- Prompt layout (spec, "Context layout and KV cache"):
  - **System message:** `engineRules + "\n\n# World\n" + rules`. If any lore is always-on, add `"# Lore\n- subject: data\n..."`. If any summaries exist, add `"# Story so far\nTurns a-b: text\n..."`.
  - **Transcript:** the messages from L3.
  - **Final user message:** `"# Current state\n" + view`. If there are retrieved snippets, add `"\n# Relevant\n- ...\n"`. Then `"\n# Player\n" + input + "\n\n# Task\n" + instruction`.

- [ ] **Step 1: Write the failing test `tests/context/PromptBuilderTests.cpp`**

```cpp
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
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'context/PromptBuilder.h'`.

- [ ] **Step 3: Create `src/context/PromptBuilder.h`**

```cpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"

namespace morpheus::context {

// What goes into one resolve prompt, by layer (architecture spec, "Context layout and KV cache"). The world
// (L1) is passed separately.
struct ContextInputs {
    std::string engineRules = {};                  // L0: engine rules and the output format.
    std::vector<core::Summary> summaries = {};     // L2: folded turns, oldest first.
    std::vector<core::TurnRecord> transcript = {}; // L3: unfolded turns, oldest first.
    nlohmann::json state = nullptr;                // T1: current state (validated).
    std::vector<std::string> retrieved = {};       // T2: formatted lore/history snippets.
    std::string playerInput = {};                  // T3
    std::string instruction = {};                  // T4: the resolve task instruction.
};

// Exact token counts. Layer counts are raw text (no template tokens); `total` is the whole templated prompt.
struct LayerTokens {
    int engineRules = 0;
    int world = 0;
    int summary = 0;
    int transcript = 0;
    int tail = 0;
    int total = 0;
};

struct Assembly {
    inference::Prompt prompt = {};
    LayerTokens tokens = {};
    int transcriptBudget = 0;          // Tokens L3 may use under this budget.
    bool transcriptOverBudget = false; // Fold old turns into the summary and assemble again.
    bool summaryOverBudget = false;    // The summary itself must be condensed.
    bool tailOverBudget = false;       // State, retrieval, input and instruction exceed tail_allowance.
    bool contextOverflow = false;      // The templated prompt plus both generations exceeds the context.

    bool Fits() const
    {
        return !transcriptOverBudget && !summaryOverBudget && !tailOverBudget && !contextOverflow;
    }
};

// Builds the resolve prompt: one system message (L0 + L1 + L2), the transcript (L3), then one user message with
// the volatile tail (T1-T4). prompt.stableCount marks the end of L3. Throws ContractError if L0 exceeds
// engine_rules_cap (engine text too long) and DataError if L1 exceeds world_cap (the world is too big). Other
// overruns are flags for the turn layer to act on.
Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend);

// Turns a resolve prompt into the narrate prompt by appending the resolve output (assistant) and the resolution
// record with the narrate instruction (user). It only appends, so the whole resolve prompt stays cached.
void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction);

} // namespace morpheus::context
```

- [ ] **Step 4: Create `src/context/PromptBuilder.cpp`, and add it to `src/context/CMakeLists.txt`**

```cpp
#include "context/PromptBuilder.h"

#include <algorithm>
#include <format>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "core/Errors.h"

namespace morpheus::context {

Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.engineRules.empty() && !inputs.playerInput.empty() && !inputs.instruction.empty(),
                     "AssembleResolvePrompt: engine rules, player input and instruction are required");

    // L1: the world's rules, then its always-on lore.
    std::string worldText = "# World\n" + world.rules;
    if (!worldText.ends_with('\n')) {
        worldText += '\n';
    }
    std::string lore;
    for (const core::LoreEntry& entry : world.lore) {
        if (entry.alwaysOn) {
            lore += std::format("- {}: {}\n", entry.subject, entry.data);
        }
    }
    if (!lore.empty()) {
        worldText += "# Lore\n" + lore;
    }

    // L2: folded summaries, oldest first.
    std::string summaryText;
    if (!inputs.summaries.empty()) {
        summaryText = "# Story so far\n";
        for (const core::Summary& summary : inputs.summaries) {
            summaryText += std::format("Turns {}-{}: {}\n", summary.fromTurn, summary.toTurn, summary.text);
        }
    }

    // T1-T4: rebuilt every call, so it goes last.
    std::string tail = "# Current state\n" + RenderStateView(inputs.state);
    if (!inputs.retrieved.empty()) {
        tail += "\n# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
    }
    tail += "\n# Player\n" + inputs.playerInput + "\n\n# Task\n" + inputs.instruction;

    // Messages: stable prefix (system, transcript), then the tail.
    Assembly assembly;
    inference::Prompt& prompt = assembly.prompt;
    prompt.messages.push_back({inference::Role::System, inputs.engineRules + "\n\n" + worldText + summaryText});
    for (inference::Message& message : RenderTranscript(inputs.transcript)) {
        prompt.messages.push_back(std::move(message));
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, tail});

    // Exact counts with the model's tokenizer.
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.engineRules);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    for (std::size_t i = 1; i < prompt.stableCount; ++i) {
        tokens.transcript += backend.CountTokens(prompt.messages[i].content);
    }
    tokens.tail = backend.CountTokens(tail);
    tokens.total = backend.CountPromptTokens(prompt);

    // Fixed layers: L0 is engine text (a bug if too long), L1 is world data (the author must trim it).
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("engine rules use {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("world '{}': rules and always-on lore use {} tokens, over world_cap {}",
                                          world.id, tokens.world, budget.worldCap));
    }
    assembly.summaryOverBudget = tokens.summary > budget.summaryCap;
    assembly.tailOverBudget = tokens.tail > budget.tailAllowance;

    // L3 gets what the fixed layers and the reserved generations leave.
    assembly.transcriptBudget = backend.ContextSize() - budget.resolveMaxTokens - budget.narrateMaxTokens -
                                budget.tailAllowance - tokens.engineRules - tokens.world -
                                std::min(tokens.summary, budget.summaryCap);
    assembly.transcriptOverBudget = tokens.transcript > assembly.transcriptBudget;

    // Template tokens are not in the layer counts; this catches them overflowing the context.
    assembly.contextOverflow =
        tokens.total + budget.resolveMaxTokens + budget.narrateMaxTokens > backend.ContextSize();
    return assembly;
}

void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction)
{
    MORPHEUS_REQUIRE(!prompt.messages.empty() && prompt.messages.back().role == inference::Role::User,
                     "ExtendForNarrate: expects a resolve prompt ending with the user tail");
    prompt.messages.push_back({inference::Role::Assistant, resolveOutput});
    prompt.messages.push_back({inference::Role::User, resolutionAndInstruction});
}

} // namespace morpheus::context
```

Add `PromptBuilder.cpp` to the `add_library(morpheus_context ...)` source list.

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass. If the mini world's rules or lore make a budget section's numbers land differently, adjust only that section's sizes so that it exercises the intended branch, and say so in the report.

- [ ] **Step 6: Commit**

```bash
git add src/context tests/context
git commit -m "Assemble layered prompts with exact token budgets" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

