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
    std::string checkEntity = {};                 // Entity whose stats a check adds (see `stats`).
    std::vector<std::string> stats = {};          // Int fields of the check entity usable in checks, in world.json order.
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
