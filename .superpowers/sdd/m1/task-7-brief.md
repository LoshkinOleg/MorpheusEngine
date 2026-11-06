### Task 7: Convert the sandcrawler world

**Files:**
- Rename: `game_projects/sandcrawler/lore/default_lore_entries.csv` -> `game_projects/sandcrawler/lore.csv`
- Rename and edit: `game_projects/sandcrawler/system/instructions.md` -> `game_projects/sandcrawler/rules.md`
- Create: `game_projects/sandcrawler/world.json`, `state_schema.json`, `initial_state.json`
- Delete: `game_projects/sandcrawler/manifest.json`, `game_projects/sandcrawler/system/agent_prompt.md`
- Create: `tests/core/SandcrawlerWorldTests.cpp`

**Interfaces:**
- Consumes: `LoadWorld`, `World`, `EntityTypeDef` (Tasks 3 and 6), `morpheus::test::RepoDir()` (Task 1).
- Produces: a loadable `game_projects/sandcrawler/` world. Entity ids are `player` (the check entity), `crawler` and `world`. Created NPCs use type `npc`. The stats added to checks are the player's `grit`, `wits`, `presence` and `agility`.
- Note for the repository owner: the stats, resources and starting values below are a first pass based on the lore entries "player starting conditions" and "small sandcrawler". Tune them freely. They are data, not code.

- [ ] **Step 1: Write the failing test `tests/core/SandcrawlerWorldTests.cpp`**

```cpp
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
```

- [ ] **Step 2: Build and run it. Expect failure.**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[sandcrawler]"`
Expected: FAIL with a `DataError` message containing `cannot read` and `world.json`.

- [ ] **Step 3: Move and delete the old files**

```bash
git mv game_projects/sandcrawler/lore/default_lore_entries.csv game_projects/sandcrawler/lore.csv
git mv game_projects/sandcrawler/system/instructions.md game_projects/sandcrawler/rules.md
git rm game_projects/sandcrawler/manifest.json game_projects/sandcrawler/system/agent_prompt.md
```

- [ ] **Step 4: Edit `game_projects/sandcrawler/rules.md`**

Replace the whole file with the following. Only the Lore and Adjudication paragraphs change, to match the engine: lore is supplied as entries, and the engine rolls the dice and tracks resources as state.

```markdown
You are the GM for an ongoing solo tabletop RPG set on a desert planet of nomadic sandcrawlers. The player controls one character; you control everything else.

**Lore.** Canonical lore is given to you as reference entries. Treat it as ground truth. NEVER invent facts that contradict existing lore. If unsure whether something is established, ask rather than assume.

**Pushback.** If the player attempts something that contradicts lore, physics, or established character capability, resist in-fiction first — the world doesn't bend, the NPC refuses, the action fails. Break character only if they seem genuinely confused about the rules of the world. The player character is not specially destined. NPCs have their own agendas and may dislike, ignore, or oppose the character for valid reasons.

**Narration.** Second person, present tense. Tight prose; cut purple description unless the moment earns it. End scenes on a decision point. Distinguish player knowledge from character knowledge — if the character wouldn't know it, don't reveal it.

**Adjudication.** When outcomes are uncertain, name the stakes and call for a check; the engine rolls the dice and reports the result. Don't fudge results — failure is interesting. Injuries, time, fuel, water, and ammunition persist across scenes; the engine tracks them as state.
```

- [ ] **Step 5: Create `game_projects/sandcrawler/world.json`**

```json
{
  "id": "sandcrawler",
  "title": "Sandcrawler Captain",
  "dice": "1d20+stat",
  "check_entity": "player",
  "difficulties": { "easy": 8, "medium": 12, "hard": 16, "extreme": 20 },
  "memory_mode": "engine"
}
```

- [ ] **Step 6: Create `game_projects/sandcrawler/state_schema.json`**

```json
{
  "entity_types": {
    "captain": {
      "fields": {
        "health": { "type": "int", "min": 0, "max": 10, "default": 10 },
        "conditions": { "type": "list" },
        "location": { "type": "string" },
        "inventory": { "type": "counter" },
        "grit": { "type": "int", "min": 0, "max": 5 },
        "wits": { "type": "int", "min": 0, "max": 5 },
        "presence": { "type": "int", "min": 0, "max": 5 },
        "agility": { "type": "int", "min": 0, "max": 5 }
      }
    },
    "crawler": {
      "fields": {
        "fuel": { "type": "int", "min": 0, "max": 100 },
        "water": { "type": "int", "min": 0, "max": 100 },
        "food": { "type": "int", "min": 0, "max": 100 },
        "ammunition": { "type": "int", "min": 0, "max": 100 },
        "hull": { "type": "int", "min": 0, "max": 100, "default": 100 },
        "crew": { "type": "int", "min": 0, "max": 40 },
        "morale": { "type": "enum", "values": ["steady", "mutinous", "low", "high"] },
        "location": { "type": "string" },
        "cargo": { "type": "counter" },
        "vehicles": { "type": "counter" }
      }
    },
    "world": {
      "fields": {
        "day": { "type": "int", "min": 1, "max": 100000 },
        "time_of_day": { "type": "enum", "values": ["dawn", "morning", "noon", "afternoon", "dusk", "night"] }
      }
    },
    "npc": {
      "creatable": true,
      "fields": {
        "role": { "type": "string" },
        "disposition": { "type": "enum", "values": ["neutral", "hostile", "wary", "friendly", "loyal"] },
        "location": { "type": "string" },
        "conditions": { "type": "list" },
        "health": { "type": "int", "min": 0, "max": 10, "default": 10 }
      }
    }
  }
}
```

- [ ] **Step 7: Create `game_projects/sandcrawler/initial_state.json`**

```json
{
  "player": {
    "type": "captain",
    "fields": {
      "health": 10,
      "conditions": [],
      "location": "open desert",
      "inventory": { "pistol": 1, "compass": 1 },
      "grit": 2,
      "wits": 3,
      "presence": 2,
      "agility": 1
    }
  },
  "crawler": {
    "type": "crawler",
    "fields": {
      "fuel": 60,
      "water": 50,
      "food": 50,
      "ammunition": 40,
      "hull": 100,
      "crew": 20,
      "morale": "steady",
      "location": "open desert",
      "cargo": {},
      "vehicles": { "sand_bike": 2 }
    }
  },
  "world": {
    "type": "world",
    "fields": { "day": 1, "time_of_day": "dawn" }
  }
}
```

- [ ] **Step 8: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `The shipped sandcrawler world loads`.

- [ ] **Step 9: Commit**

```bash
git add -A game_projects/sandcrawler tests/core/SandcrawlerWorldTests.cpp
git commit -m "Convert sandcrawler to the world folder format" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

