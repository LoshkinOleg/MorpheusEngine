#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/MemoryMode.h"

namespace morpheus::core {

struct SamplingSettings {
    double temperature = 0.0; // 0 means greedy decoding.
    int topK = 40;
    double topP = 0.95;
};

// Token budget for one assembled prompt (architecture spec, "Context layout and KV cache").
struct BudgetSettings {
    int resolveMaxTokens = 0; // Generation reserved for the resolve call.
    int narrateMaxTokens = 0; // Generation reserved for the narrate call.
    int narrateAllowance = 0; // Resolution record + narrate instruction appended for the narrate call.
    int tailAllowance = 0;    // State view + retrieved snippets + player input + instruction (T1-T4).
    int engineRulesCap = 0;   // L0
    int worldCap = 0;         // L1: world rules + always-on lore.
    int summaryCap = 0;       // L2
    int retrievalCap = 0;     // T2: retrieved lore, history and notes. Part of the tail allowance.
};

// settings.json: per-machine configuration. Not committed; settings.example.json documents it.
struct Settings {
    std::filesystem::path chatModel = {};      // Relative paths are resolved against the settings file's folder.
    std::filesystem::path embeddingModel = {};
    std::string embeddingQueryPrefix = {};     // Prepended to retrieval queries (model-specific).
    std::string embeddingDocumentPrefix = {};  // Prepended to embedded documents (model-specific).
    int contextSize = 0;
    int gpuLayers = 0;                         // -1 puts every layer on the GPU.
    BudgetSettings budget = {};
    SamplingSettings resolveSampling = {};
    SamplingSettings narrateSampling = {};
    std::optional<MemoryMode> memoryMode = {}; // Overrides the world's default memory_mode when set.
};

// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors,
// both model files must exist, and the budget must leave room for the transcript (L3) and SETTINGS_TEMPLATE_MARGIN
// chat-template tokens. memory_mode is optional.
Settings LoadSettings(const std::filesystem::path& file);

// The settings as settings.json JSON, with model paths absolute (UTF-8). Stored in a run's meta for reference.
nlohmann::json SettingsToJson(const Settings& settings);

} // namespace morpheus::core
