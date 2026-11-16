#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Dice.h"
#include "core/MemoryMode.h"
#include "core/StateSchema.h"

namespace morpheus::core {

// A host lore entry's condition (host-feedback spec, section 2): a value of the turn's observation, found by a JSON
// pointer, that must meet every comparison given.
struct LoreCondition {
    std::string pointer = {};                  // A well-formed JSON pointer into the observation.
    std::optional<double> min = {};            // Inclusive; the value must be a number.
    std::optional<double> max = {};            // Inclusive; the value must be a number.
    std::optional<nlohmann::json> equals = {}; // Any JSON, compared with JSON equality.

    // True when `observation` holds a value at the pointer that meets every comparison given. A missing value, or a
    // value of another type than a comparison needs, does not hold. Throws ContractError for a pointer that is not
    // well-formed: the host's data is checked at open, so none reaches here.
    bool Holds(const nlohmann::json& observation) const;
};

struct LoreEntry {
    std::string subject = {};
    std::string data = {};
    bool alwaysOn = false; // Always in the prompt (layer L1) instead of retrieved when relevant.
    // Hosted runs only (lore.csv sets neither). Empty scopes: every speaker may retrieve the entry; otherwise only a
    // speaker whose `knows` shares one.
    std::vector<std::string> scopes = {};
    std::optional<LoreCondition> condition = {}; // The entry is visible only while it holds on the observation.
};

// world.json's optional "scene": the state values that describe where the player is, for scene retrieval. Each path
// is "entity.field", a string field of an entity of the initial state; presentAt is one of them: every other entity
// whose field of that name holds the same value is present (turn::SceneOf).
struct SceneDecl {
    std::vector<std::string> paths = {};
    std::string presentAt = {};
};

// Everything in one game_projects/<id>/ folder, validated.
struct World {
    std::string id = {};
    std::string title = {};
    DiceRule dice = {};
    std::map<std::string, int> difficulties = {}; // Difficulty name -> target number.
    std::string checkEntity = {};                 // Entity whose stats a check adds (see `stats`).
    std::vector<std::string> stats = {};          // Int fields of the check entity usable in checks, in world.json order.
    std::optional<SceneDecl> scene = {}; // Absent: retrieval searches with the input and the previous digest only.
    MemoryMode defaultMemoryMode = MemoryMode::Engine;
    std::string rules = {};
    std::vector<LoreEntry> lore = {}; // In file order.
    StateSchema schema = {};
    nlohmann::json initialState = nullptr;
    std::string contentHash = {}; // FNV-1a over every world file, see LoadWorld.
};

// Loads a world folder: world.json, rules.md, lore.csv, state_schema.json, initial_state.json.
// Throws DataError naming the file (and field) at fault. world.json's optional "scene" is checked against the schema
// and the initial state.
World LoadWorld(const std::filesystem::path& directory);

} // namespace morpheus::core
