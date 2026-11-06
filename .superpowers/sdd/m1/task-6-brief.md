### Task 6: World loading

**Files:**
- Create: `src/core/World.h`, `src/core/World.cpp`, `tests/core/WorldTests.cpp`
- Create: `tests/data/worlds/mini/world.json`, `rules.md`, `lore.csv`, `state_schema.json`, `initial_state.json`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `ContentHash` (Task 1), `ParseCsv` (Task 2), `StateSchema` (Task 3), `DiceRule` (Task 5), `DataError`.
- Produces (namespace `morpheus::core`):
  - `enum class MemoryMode { Engine, Assisted }`
  - `struct LoreEntry { std::string subject; std::string data; bool alwaysOn; }`
  - `struct World { id, title, dice, difficulties, checkEntity, defaultMemoryMode, rules, lore, schema, initialState, contentHash }`
  - `World LoadWorld(const std::filesystem::path& directory)`
- **`world.json` format** (used by Task 7):
  `{"id": "<folder name>", "title": "...", "dice": "1d20+stat", "check_entity": "<entity id>", "difficulties": {"<name>": <int>}, "memory_mode": "engine" | "assisted"}`

- [ ] **Step 1: Create the test world `tests/data/worlds/mini/`**

`tests/data/worlds/mini/world.json`:

```json
{
  "id": "mini",
  "title": "Mini World",
  "dice": "1d20+stat",
  "check_entity": "player",
  "difficulties": { "easy": 8, "hard": 16 },
  "memory_mode": "engine"
}
```

`tests/data/worlds/mini/rules.md`:

```markdown
You are the GM of a tiny test world.
```

`tests/data/worlds/mini/lore.csv`:

```
subject,data,always_on
lighthouse,"A tall lighthouse, painted red.",true
keeper,"The keeper says ""hello"" to everyone.",false
storm,Storms arrive at dusk.,
```

`tests/data/worlds/mini/state_schema.json`:

```json
{
  "entity_types": {
    "character": {
      "fields": {
        "health": { "type": "int", "min": 0, "max": 10 },
        "agility": { "type": "int", "min": 0, "max": 5 },
        "location": { "type": "string" },
        "inventory": { "type": "counter" }
      }
    },
    "npc": {
      "creatable": true,
      "fields": {
        "disposition": { "type": "enum", "values": ["wary", "friendly"] }
      }
    }
  }
}
```

`tests/data/worlds/mini/initial_state.json`:

```json
{
  "player": {
    "type": "character",
    "fields": { "health": 10, "agility": 3, "location": "shore", "inventory": { "lantern": 1 } }
  }
}
```

- [ ] **Step 2: Write the failing test `tests/core/WorldTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <map>
#include <string>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/World.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

fs::path MiniWorldDir()
{
    return morpheus::test::TestDataDir() / "worlds" / "mini";
}

// Copies the mini world into a fresh temp folder so a test can break one file. `folderName` is the copy's
// folder name, which LoadWorld compares with the world id.
fs::path CopyMiniWorld(const std::string& testName, const std::string& folderName = "mini")
{
    const fs::path dir = morpheus::test::FreshTempDir(testName) / folderName;
    fs::copy(MiniWorldDir(), dir, fs::copy_options::recursive);
    return dir;
}

// world.json for the mini world with one key overridden.
std::string MiniWorldJsonWith(const std::string& key, const std::string& valueJson)
{
    nlohmann::json json = nlohmann::json::parse(R"({
        "id": "mini", "title": "Mini World", "dice": "1d20+stat", "check_entity": "player",
        "difficulties": {"easy": 8, "hard": 16}, "memory_mode": "engine"
    })");
    json[key] = nlohmann::json::parse(valueJson);
    return json.dump();
}

} // namespace

TEST_CASE("LoadWorld loads every file of a valid world", "[world]")
{
    const World world = LoadWorld(MiniWorldDir());

    CHECK(world.id == "mini");
    CHECK(world.title == "Mini World");
    CHECK(world.dice.sides == 20);
    CHECK(world.dice.addStat);
    CHECK(world.checkEntity == "player");
    CHECK(world.difficulties == std::map<std::string, int>{{"easy", 8}, {"hard", 16}});
    CHECK(world.defaultMemoryMode == MemoryMode::Engine);
    CHECK_THAT(world.rules, ContainsSubstring("tiny test world"));

    REQUIRE(world.lore.size() == 3);
    CHECK(world.lore[0].subject == "lighthouse");
    CHECK(world.lore[0].data == "A tall lighthouse, painted red.");
    CHECK(world.lore[0].alwaysOn);
    CHECK(world.lore[1].data == "The keeper says \"hello\" to everyone.");
    CHECK_FALSE(world.lore[1].alwaysOn);
    CHECK_FALSE(world.lore[2].alwaysOn);

    CHECK(world.schema.FindEntityType("npc") != nullptr);
    CHECK(world.initialState.at("player").at("fields").at("health") == 10);
    CHECK(world.contentHash.size() == 16);
}

TEST_CASE("LoadWorld content hash tracks file contents", "[world]")
{
    const fs::path dir = CopyMiniWorld("world_hash");
    const std::string before = LoadWorld(dir).contentHash;
    CHECK(LoadWorld(dir).contentHash == before);

    morpheus::test::WriteFile(dir / "rules.md", "Different rules.\n");
    CHECK(LoadWorld(dir).contentHash != before);
}

TEST_CASE("LoadWorld reports the file at fault", "[world]")
{
    SECTION("missing file")
    {
        const fs::path dir = CopyMiniWorld("world_missing");
        fs::remove(dir / "lore.csv");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("lore.csv"));
    }
    SECTION("id does not match the folder")
    {
        CHECK_THROWS_WITH(LoadWorld(CopyMiniWorld("world_id", "renamed")), ContainsSubstring("folder name"));
    }
    SECTION("malformed JSON")
    {
        const fs::path dir = CopyMiniWorld("world_json");
        morpheus::test::WriteFile(dir / "world.json", "{ not json");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("world.json"));
    }
    SECTION("unknown memory mode")
    {
        const fs::path dir = CopyMiniWorld("world_memory_mode");
        morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("memory_mode", "\"chaos\""));
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("memory_mode"));
    }
    SECTION("check entity not in the initial state")
    {
        const fs::path dir = CopyMiniWorld("world_check_entity");
        morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("check_entity", "\"hero\""));
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("check_entity"));
    }
    SECTION("bad lore header")
    {
        const fs::path dir = CopyMiniWorld("world_lore_header");
        morpheus::test::WriteFile(dir / "lore.csv", "name,text\nx,y\n");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("lore.csv"));
    }
    SECTION("duplicate lore subject")
    {
        const fs::path dir = CopyMiniWorld("world_lore_duplicate");
        morpheus::test::WriteFile(dir / "lore.csv", "subject,data\nx,one\nx,two\n");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("duplicate"));
    }
    SECTION("invalid initial state")
    {
        const fs::path dir = CopyMiniWorld("world_initial_state");
        morpheus::test::WriteFile(dir / "initial_state.json",
            R"({"player": {"type": "character", "fields": {"health": 99, "agility": 3, "location": "shore", "inventory": {}}}})");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("health"));
    }
}
```

- [ ] **Step 3: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/World.h'`.

- [ ] **Step 4: Implement `src/core/World.h`**

```cpp
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Dice.h"
#include "core/StateSchema.h"

namespace morpheus::core {

// Who manages memory beyond the engine's deterministic retrieval (architecture spec, turn flow).
enum class MemoryMode { Engine, Assisted };

struct LoreEntry {
    std::string subject = {};
    std::string data = {};
    bool alwaysOn = false; // Always in the prompt (layer L1) instead of retrieved when relevant.
};

// Everything in one game_projects/<id>/ folder, validated.
struct World {
    std::string id = {};
    std::string title = {};
    DiceRule dice = {};
    std::map<std::string, int> difficulties = {}; // Difficulty name -> target number.
    std::string checkEntity = {};                 // Entity whose int fields are the stats that checks add.
    MemoryMode defaultMemoryMode = MemoryMode::Engine;
    std::string rules = {};
    std::vector<LoreEntry> lore = {}; // In file order.
    StateSchema schema = {};
    nlohmann::json initialState = nullptr;
    std::string contentHash = {}; // FNV-1a over every world file, see LoadWorld.
};

// Loads a world folder: world.json, rules.md, lore.csv, state_schema.json, initial_state.json.
// Throws DataError naming the file (and field) at fault.
World LoadWorld(const std::filesystem::path& directory);

} // namespace morpheus::core
```

- [ ] **Step 5: Implement `src/core/World.cpp`, and add `World.cpp` to `src/core/CMakeLists.txt` after `Dice.cpp`**

```cpp
#include "core/World.h"

#include <array>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include "core/ContentHash.h"
#include "core/Csv.h"
#include "core/Errors.h"

namespace morpheus::core {

namespace {

// Reads a whole file as raw bytes. Throws DataError when it cannot be opened.
std::string ReadTextFile(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw DataError(std::format("cannot read '{}'", file.generic_string()));
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Parses JSON text, turning parse errors into DataError prefixed with the file name.
nlohmann::json ParseJsonFile(const std::filesystem::path& file, const std::string& text)
{
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        throw DataError(std::format("'{}': {}", file.generic_string(), e.what()));
    }
}

} // namespace

World LoadWorld(const std::filesystem::path& directory)
{
    // A trailing separator would leave filename() empty; normalize so the folder-name check works.
    std::filesystem::path dir = directory.lexically_normal();
    if (dir.filename().empty()) {
        dir = dir.parent_path();
    }
    const std::string folderName = dir.filename().string();

    // Read every file up front: the content hash covers all of them, names included, in this fixed order.
    const std::array<std::string, 5> FILE_NAMES = {
        "world.json", "rules.md", "lore.csv", "state_schema.json", "initial_state.json"};
    std::map<std::string, std::string> texts;
    ContentHash hash;
    for (const std::string& name : FILE_NAMES) {
        std::string text = ReadTextFile(dir / name);
        hash.Add(name);
        hash.Add(text);
        texts.emplace(name, std::move(text));
    }

    World world;
    world.contentHash = hash.Hex();

    // world.json: identity, dice, difficulties, defaults.
    const std::filesystem::path worldFile = dir / "world.json";
    const std::string worldWhere = std::format("'{}'", worldFile.generic_string());
    {
        const nlohmann::json json = ParseJsonFile(worldFile, texts.at("world.json"));
        if (!json.is_object()) {
            throw DataError(worldWhere + ": must be a JSON object");
        }
        auto requireString = [&](const char* key) {
            if (!json.contains(key) || !json.at(key).is_string() || json.at(key).get<std::string>().empty()) {
                throw DataError(std::format("{}: needs a non-empty string '{}'", worldWhere, key));
            }
            return json.at(key).get<std::string>();
        };

        world.id = requireString("id");
        if (world.id != folderName) {
            throw DataError(std::format("{}: id '{}' must match the folder name '{}'", worldWhere, world.id, folderName));
        }
        world.title = requireString("title");

        const std::string diceText = requireString("dice");
        try {
            world.dice = DiceRule::Parse(diceText);
        } catch (const DataError& e) {
            throw DataError(std::format("{}: {}", worldWhere, e.what()));
        }

        world.checkEntity = requireString("check_entity");

        const std::string memoryMode = requireString("memory_mode");
        if (memoryMode == "engine") {
            world.defaultMemoryMode = MemoryMode::Engine;
        } else if (memoryMode == "assisted") {
            world.defaultMemoryMode = MemoryMode::Assisted;
        } else {
            throw DataError(std::format("{}: memory_mode must be 'engine' or 'assisted', got '{}'", worldWhere, memoryMode));
        }

        if (!json.contains("difficulties") || !json.at("difficulties").is_object() || json.at("difficulties").empty()) {
            throw DataError(worldWhere + ": needs a non-empty 'difficulties' object");
        }
        for (const auto& [name, target] : json.at("difficulties").items()) {
            if (!target.is_number_integer()) {
                throw DataError(std::format("{}: difficulty '{}' must be an integer", worldWhere, name));
            }
            world.difficulties.emplace(name, target.get<int>());
        }
    }

    // rules.md: the GM instructions, used verbatim.
    world.rules = texts.at("rules.md");
    if (world.rules.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw DataError(std::format("'{}': must not be empty", (dir / "rules.md").generic_string()));
    }

    // lore.csv: header "subject,data" or "subject,data,always_on", then one entry per record.
    {
        const std::string loreWhere = std::format("'{}'", (dir / "lore.csv").generic_string());
        std::vector<std::vector<std::string>> records;
        try {
            records = ParseCsv(texts.at("lore.csv"));
        } catch (const DataError& e) {
            throw DataError(std::format("{}: {}", loreWhere, e.what()));
        }

        const std::vector<std::string> plainHeader = {"subject", "data"};
        const std::vector<std::string> alwaysOnHeader = {"subject", "data", "always_on"};
        if (records.empty() || (records.front() != plainHeader && records.front() != alwaysOnHeader)) {
            throw DataError(loreWhere + ": header must be subject,data or subject,data,always_on");
        }
        const bool hasAlwaysOn = records.front() == alwaysOnHeader;

        std::set<std::string> subjects;
        for (std::size_t i = 1; i < records.size(); ++i) {
            const std::vector<std::string>& record = records[i];
            const std::string recordWhere = std::format("{} record {}", loreWhere, i + 1);
            if (record.size() != records.front().size()) {
                throw DataError(std::format("{}: expected {} fields, got {}", recordWhere, records.front().size(), record.size()));
            }

            LoreEntry entry;
            entry.subject = record[0];
            entry.data = record[1];
            if (entry.subject.empty() || entry.data.empty()) {
                throw DataError(recordWhere + ": subject and data must not be empty");
            }
            if (!subjects.insert(entry.subject).second) {
                throw DataError(std::format("{}: duplicate subject '{}'", recordWhere, entry.subject));
            }
            // always_on: "true", "false" or empty (= false).
            if (hasAlwaysOn) {
                if (record[2] == "true") {
                    entry.alwaysOn = true;
                } else if (record[2] != "false" && !record[2].empty()) {
                    throw DataError(recordWhere + ": always_on must be true, false or empty");
                }
            }
            world.lore.push_back(std::move(entry));
        }
    }

    // state_schema.json, then initial_state.json validated against it.
    {
        const std::filesystem::path file = dir / "state_schema.json";
        const nlohmann::json json = ParseJsonFile(file, texts.at("state_schema.json"));
        try {
            world.schema = StateSchema::FromJson(json);
        } catch (const DataError& e) {
            throw DataError(std::format("'{}': {}", file.generic_string(), e.what()));
        }
    }
    {
        const std::filesystem::path file = dir / "initial_state.json";
        world.initialState = ParseJsonFile(file, texts.at("initial_state.json"));
        try {
            world.schema.ValidateState(world.initialState);
        } catch (const DataError& e) {
            throw DataError(std::format("'{}': {}", file.generic_string(), e.what()));
        }
    }

    // Checks add stats from this entity, so it has to exist from the start.
    if (!world.initialState.contains(world.checkEntity)) {
        throw DataError(std::format("{}: check_entity '{}' is not an entity in initial_state.json", worldWhere, world.checkEntity));
    }
    return world;
}

} // namespace morpheus::core
```

- [ ] **Step 6: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/core tests/core/WorldTests.cpp tests/data
git commit -m "Add world folder loading with content hash" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

