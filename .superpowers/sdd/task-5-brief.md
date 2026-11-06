### Task 5: Context: world reference in L1, over-cap summary flag, change parts

**Files:**
- Create: `src/context/WorldReference.h`, `src/context/WorldReference.cpp`
- Modify: `src/context/CMakeLists.txt`, `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`, `src/context/Transcript.h`, `src/context/Transcript.cpp`, `src/core/StateSchema.h`, `src/core/StateSchema.cpp`
- Modify tests: `tests/context/RenderTests.cpp`, `tests/context/PromptBuilderTests.cpp`

**Interfaces:**
- Consumes: `core::World`, `core::StateSchema`.
- Produces:
  - `const std::map<std::string, EntityTypeDef>& core::StateSchema::EntityTypes() const`
  - `std::string context::RenderWorldReference(const core::World& world)`
  - `std::string context::RenderChangePart(const core::AppliedChange& applied)`
  - Behavior:
    - L1 is the rules, then the game reference, then the always-on lore.
    - A summary over `summary_cap` sets `summaryOverBudget` and never throws the no-room `DataError`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/context/RenderTests.cpp`. Add `#include "TestPaths.h"`, `#include "context/WorldReference.h"` and `#include "core/World.h"`.

```cpp
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
```

Append to `tests/context/PromptBuilderTests.cpp`:

```cpp
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
    const core::BudgetSettings budget = RoomyBudget(); // summary_cap 256
    context::ContextInputs inputs = Inputs(world);
    inputs.summaries = {{1, 4, std::string(4000, 's')}}; // About 1000 tokens.
    const Measured measured = Measure(world, inputs, budget);
    // One token short of room for the full system message: before this change, a no-room DataError.
    test::FakeBackend backend(budget.resolveMaxTokens + budget.narrateMaxTokens + budget.narrateAllowance +
                              budget.tailAllowance + measured.systemCost - 1);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, budget, backend);
    CHECK(assembly.summaryOverBudget);
    CHECK(assembly.contextOverflow);
    CHECK(assembly.transcriptBudget > 0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `context/WorldReference.h` is missing and `RenderChangePart` is undeclared.

- [ ] **Step 3: Implement**

`src/core/StateSchema.h`, after `FindEntityType`:

```cpp
    // Every entity type, by name.
    const std::map<std::string, EntityTypeDef>& EntityTypes() const;
```

`src/core/StateSchema.cpp`:

```cpp
const std::map<std::string, EntityTypeDef>& StateSchema::EntityTypes() const
{
    return _entityTypes;
}
```

Create `src/context/WorldReference.h`:

```cpp
#pragma once

#include <string>

#include "core/World.h"

namespace morpheus::context {

// The world's game reference for L1: how checks roll, the difficulties, the stats, and every entity type with its
// fields, so the model knows what a resolve output may name. Difficulties are in target order (ties by name),
// types and fields in name order, so the text is deterministic:
//   # Game reference
//   Checks roll 1d20+stat and succeed at or above the difficulty: easy 8, hard 16.
//   Stats (int fields of player): agility.
//   Entity types:
//   - character: agility (int 0..5), health (int 0..10), inventory (counter), location (string)
//   - npc (creatable): disposition (enum: wary, friendly)
std::string RenderWorldReference(const core::World& world);

} // namespace morpheus::context
```

Create `src/context/WorldReference.cpp`:

```cpp
#include "context/WorldReference.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>
#include <vector>

#include "core/Errors.h"

namespace morpheus::context {

std::string RenderWorldReference(const core::World& world)
{
    std::vector<std::pair<std::string, int>> difficulties(world.difficulties.begin(), world.difficulties.end());
    std::stable_sort(difficulties.begin(), difficulties.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });

    std::string text = "# Game reference\n";
    text += std::format("Checks roll {}d{}{} and succeed at or above the difficulty:", world.dice.count,
                        world.dice.sides, world.dice.addStat ? "+stat" : "");
    for (std::size_t i = 0; i < difficulties.size(); ++i) {
        text += std::format("{} {} {}", i == 0 ? "" : ",", difficulties[i].first, difficulties[i].second);
    }
    text += ".\n";

    text += std::format("Stats (int fields of {}):", world.checkEntity);
    for (std::size_t i = 0; i < world.stats.size(); ++i) {
        text += (i == 0 ? " " : ", ") + world.stats[i];
    }
    text += ".\nEntity types:\n";

    for (const auto& [typeName, type] : world.schema.EntityTypes()) {
        text += std::format("- {}{}:", typeName, type.creatable ? " (creatable)" : "");
        bool first = true;
        for (const auto& [fieldName, def] : type.fields) {
            text += first ? " " : ", ";
            first = false;
            switch (def.type) {
            case core::FieldType::Int:
                text += std::format("{} (int {}..{})", fieldName, def.min, def.max);
                break;
            case core::FieldType::Enum: {
                std::string values;
                for (std::size_t i = 0; i < def.values.size(); ++i) {
                    values += (i == 0 ? "" : ", ") + def.values[i];
                }
                text += std::format("{} (enum: {})", fieldName, values);
                break;
            }
            case core::FieldType::String:
            case core::FieldType::List:
            case core::FieldType::Counter:
                text += std::format("{} ({})", fieldName, core::FieldTypeName(def.type));
                break;
            }
        }
        text += '\n';
    }
    return text;
}

} // namespace morpheus::context
```

The difficulty loop writes `" easy 8"`, then `", hard 16"`, which gives "difficulty: easy 8, hard 16.". Check it against the test's exact string.

Add `WorldReference.cpp` to `src/context/CMakeLists.txt`.

`src/context/PromptBuilder.cpp`: include `"context/WorldReference.h"` and `<algorithm>`.
1. L1 becomes the rules, then the reference, then the lore:

```cpp
    // L1: the world's rules, its game reference, then its always-on lore.
    std::string worldText = "# World\n" + world.rules;
    if (!worldText.ends_with('\n')) {
        worldText += '\n';
    }
    worldText += "\n" + RenderWorldReference(world);
```

   The lore block that follows is unchanged.
2. Replace the `transcriptBudget` computation and its `DataError` check:

```cpp
    // L3 gets what the templated system message, the tail allowance, the narrate allowance and both generations
    // leave, with the summary counted at most at its cap: an over-cap summary is a flag (condense it), not a
    // settings error.
    const int summaryExcess = std::max(0, tokens.summary - budget.summaryCap);
    assembly.transcriptBudget = backend.ContextSize() - budget.resolveMaxTokens - budget.narrateMaxTokens -
                                budget.narrateAllowance - budget.tailAllowance - (systemCost - summaryExcess);
```

   Keep the `if (assembly.transcriptBudget <= 0)` `DataError`, with its message naming `systemCost - summaryExcess`.

`src/context/PromptBuilder.h`:
- The `transcriptBudget` member comment becomes "Tokens L3 may add to the templated prompt, with the summary counted at most at summary_cap (always positive)."
- The `AssembleResolvePrompt` comment:
  - says L1 is "the world's rules, game reference (RenderWorldReference) and always-on lore";
  - says the no-room `DataError` counts the summary at most at its cap.

`src/context/Transcript.h`, before `RenderMechanicsLine`:

```cpp
// One applied change as the mechanics line and the resolution record show it, e.g. `player.health -3 (clamped
// to 0)`: set "path = value"; add "path +N" / "path -N" / "path +item"; remove "path -item" or "path removed";
// create "id created"; an engine note follows in parentheses.
std::string RenderChangePart(const core::AppliedChange& applied);
```

Shorten the `RenderMechanicsLine` comment to refer to `RenderChangePart` for the change forms. In `Transcript.cpp`, move the per-change switch and note logic out of `RenderMechanicsLine` into `RenderChangePart`, and call it from the loop: `parts.push_back(RenderChangePart(applied));`.

Update the existing `PromptBuilderTests` assertions that expect exact system-message text or token counts. The reference block is now part of L1, so their expected strings or counts gain it. Keep each test's intent.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[context]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/context src/core/StateSchema.h src/core/StateSchema.cpp tests/context
git commit -m "Add the world reference to L1 and flag over-cap summaries" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
