### Task 7: The world's scene block

A world may declare which state values describe where the player is (`paths`) and whose location others are compared with (`present_at`). The loader validates it against the schema and the initial state. Sandcrawler declares it; the mini test world does not.

**Files:**
- Modify: `src/core/World.h`, `src/core/World.cpp`, `game_projects/sandcrawler/world.json`
- Test: `tests/core/WorldTests.cpp`, `tests/core/SandcrawlerWorldTests.cpp`

**Interfaces:**
- Consumes: `core::StateSchema::FindEntityType`, `core::FieldType::String`.
- Produces: `struct core::SceneDecl { std::vector<std::string> paths; std::string presentAt; }`; `std::optional<core::SceneDecl> core::World::scene` (after `stats`). Every path is `entity.field` with exactly one dot, its entity in the initial state, its field a `string` field of that entity's type; `presentAt` is one of the paths.

- [ ] **Step 1: Write the failing tests**

Append to `tests/core/WorldTests.cpp`:

```cpp
TEST_CASE("LoadWorld reads an optional scene block", "[world]")
{
    CHECK_FALSE(LoadWorld(MiniWorldDir()).scene); // The mini world declares none.

    const fs::path dir = CopyMiniWorld("world_scene");
    morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("scene", R"({"paths": ["player.location"], "present_at": "player.location"})"));
    const World world = LoadWorld(dir);
    REQUIRE(world.scene);
    CHECK(world.scene->paths == std::vector<std::string>{"player.location"});
    CHECK(world.scene->presentAt == "player.location");
}

TEST_CASE("LoadWorld names world.json and the value at fault in a bad scene block", "[world]")
{
    // Each case: the scene block, and the part of the message that names what is wrong.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {R"("here")", "'scene' must be an object"},
        {R"({"paths": ["player.location"], "present_at": "player.location", "radius": 3})", "'radius'"},
        {R"({"paths": [], "present_at": "player.location"})", "'paths'"},
        {R"({"present_at": "player.location"})", "'paths'"},
        {R"({"paths": [7], "present_at": "player.location"})", "'paths'"},
        {R"({"paths": ["location"], "present_at": "location"})", "'location'"},
        {R"({"paths": ["player.location.x"], "present_at": "player.location.x"})", "'player.location.x'"},
        {R"({"paths": ["keeper.location"], "present_at": "keeper.location"})", "'keeper'"},
        {R"({"paths": ["player.health"], "present_at": "player.health"})", "'player.health'"},
        {R"({"paths": ["player.mood"], "present_at": "player.mood"})", "'player.mood'"},
        {R"({"paths": ["player.location"], "present_at": "npc.location"})", "'npc.location'"},
        {R"({"paths": ["player.location"]})", "'present_at'"},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& [scene, named] = cases[i];
        INFO("scene " << scene);
        const fs::path dir = CopyMiniWorld("world_scene_bad_" + std::to_string(i));
        morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("scene", scene));
        CHECK_THROWS_AS(LoadWorld(dir), DataError);
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("world.json") && ContainsSubstring(named));
    }
}
```

Add `#include <utility>` to the includes of `tests/core/WorldTests.cpp`.

In `tests/core/SandcrawlerWorldTests.cpp`, before the closing brace of "The shipped sandcrawler world loads", add:

```cpp
    // Scene retrieval: where the captain and the crawler are, and who is at the captain's location.
    REQUIRE(world.scene);
    CHECK(world.scene->paths == std::vector<std::string>{"player.location", "crawler.location"});
    CHECK(world.scene->presentAt == "player.location");
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: a compile error: `scene` is not a member of `morpheus::core::World`.

- [ ] **Step 3: Declare the scene**

In `src/core/World.h`, add `#include <optional>` to the includes, and after `LoreEntry` add:

```cpp
// world.json's optional "scene": the state values that describe where the player is, for scene retrieval. Each path
// is "entity.field", a string field of an entity of the initial state; presentAt is one of them: every other entity
// whose field of that name holds the same value is present (turn::SceneOf).
struct SceneDecl {
    std::vector<std::string> paths = {};
    std::string presentAt = {};
};
```

In `World`, after the `stats` member, add:

```cpp
    std::optional<SceneDecl> scene = {}; // Absent: retrieval searches with the input and the previous digest only.
```

Replace the `LoadWorld` comment with:

```cpp
// Loads a world folder: world.json, rules.md, lore.csv, state_schema.json, initial_state.json.
// Throws DataError naming the file (and field) at fault. world.json's optional "scene" is checked against the schema
// and the initial state.
```

- [ ] **Step 4: Load and validate it**

In `src/core/World.cpp`, add `#include <utility>` to the includes. Then:

Before the `// world.json: identity, dice, difficulties, defaults.` block's `{`, after `const std::string worldWhere = ...;`, add:

```cpp
    nlohmann::json sceneJson = nullptr; // world.json's "scene", checked once the schema and the state are loaded.
```

Replace the `allowedKeys` line with:

```cpp
        const std::set<std::string> allowedKeys = {"id",           "title",       "dice",       "check_entity",
                                                   "stats",        "difficulties", "memory_mode", "scene"};
```

At the end of that block (after the `difficulties` loop, before the block's closing `}`), add:

```cpp
        if (json.contains("scene")) {
            sceneJson = json.at("scene");
        }
```

After the stats loop at the end of the function, before `return world;`, add:

```cpp
    // scene: optional. Its paths name string fields of entities that exist from the start, and present_at is one of
    // them, so SceneOf can read every value each turn.
    if (!sceneJson.is_null()) {
        if (!sceneJson.is_object()) {
            throw DataError(worldWhere + ": 'scene' must be an object");
        }
        for (auto it = sceneJson.begin(); it != sceneJson.end(); ++it) {
            if (it.key() != "paths" && it.key() != "present_at") {
                throw DataError(std::format("{}: unknown key '{}' in 'scene'", worldWhere, it.key()));
            }
        }
        if (!sceneJson.contains("paths") || !sceneJson.at("paths").is_array() || sceneJson.at("paths").empty()) {
            throw DataError(worldWhere + ": 'scene' needs a non-empty 'paths' array");
        }
        SceneDecl scene;
        for (const nlohmann::json& entry : sceneJson.at("paths")) {
            if (!entry.is_string()) {
                throw DataError(worldWhere + ": 'scene' 'paths' entries must be strings");
            }
            const std::string path = entry.get<std::string>();
            // Exactly "entity.field": one dot, with text on both sides.
            const std::size_t dot = path.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 == path.size() ||
                path.find('.', dot + 1) != std::string::npos) {
                throw DataError(std::format("{}: scene path '{}' must be 'entity.field'", worldWhere, path));
            }
            const std::string entity = path.substr(0, dot);
            const std::string field = path.substr(dot + 1);
            if (!world.initialState.contains(entity)) {
                throw DataError(std::format("{}: scene path '{}': entity '{}' is not in initial_state.json", worldWhere,
                                            path, entity));
            }
            const EntityTypeDef* pType =
                world.schema.FindEntityType(world.initialState.at(entity).at("type").get<std::string>());
            MORPHEUS_REQUIRE(pType != nullptr, "LoadWorld: scene entity type missing after ValidateState");
            const auto fieldIt = pType->fields.find(field);
            if (fieldIt == pType->fields.end() || fieldIt->second.type != FieldType::String) {
                throw DataError(std::format("{}: scene path '{}': '{}' is not a string field of entity '{}'",
                                            worldWhere, path, field, entity));
            }
            scene.paths.push_back(path);
        }
        if (!sceneJson.contains("present_at") || !sceneJson.at("present_at").is_string()) {
            throw DataError(worldWhere + ": 'scene' needs a string 'present_at'");
        }
        scene.presentAt = sceneJson.at("present_at").get<std::string>();
        if (std::find(scene.paths.begin(), scene.paths.end(), scene.presentAt) == scene.paths.end()) {
            throw DataError(std::format("{}: scene 'present_at' '{}' is not one of 'paths'", worldWhere, scene.presentAt));
        }
        world.scene = std::move(scene);
    }
```

- [ ] **Step 5: Declare sandcrawler's scene**

Replace `game_projects/sandcrawler/world.json` with:

```json
{
  "id": "sandcrawler",
  "title": "Sandcrawler Captain",
  "dice": "1d20+stat",
  "check_entity": "player",
  "stats": ["grit", "wits", "presence", "agility"],
  "difficulties": { "easy": 8, "medium": 12, "hard": 16, "extreme": 20 },
  "memory_mode": "engine",
  "scene": {
    "paths": ["player.location", "crawler.location"],
    "present_at": "player.location"
  }
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[world]"`
Expected: all pass, `[sandcrawler]` included.

Then: `ctest --preset tests`
Expected: all pass (the sandcrawler replay loads the edited world; nothing reads the scene yet).

- [ ] **Step 7: Commit**

```bash
git add src/core/World.h src/core/World.cpp game_projects/sandcrawler/world.json tests/core/WorldTests.cpp tests/core/SandcrawlerWorldTests.cpp
git commit -m "Let a world declare its scene" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

