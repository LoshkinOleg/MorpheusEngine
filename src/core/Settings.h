#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/MemoryMode.h"

namespace morpheus::core {

// Smallest non-zero thinking budget: the backend closes a block that runs out with a sentence of its own, which
// must fit inside the budget.
constexpr int MIN_THINKING_TOKENS = 64;

// Which backend the app and the tools build: llama.cpp in-process, or llama-server over HTTP.
enum class BackendKind { LlamaCpp, LlamaServer };

// "llama-cpp" or "llama-server"; nullopt for anything else.
std::optional<BackendKind> ParseBackendKind(std::string_view text);

// The settings.json spelling: "llama-cpp" or "llama-server".
std::string BackendKindName(BackendKind kind);

struct SamplingSettings {
    double temperature = 0.0; // 0 means greedy decoding.
    int topK = 40;
    double topP = 0.95;
};

// Token budget for one assembled prompt (architecture spec, "Context layout and KV cache").
struct BudgetSettings {
    int resolveMaxTokens = 0; // The resolve call's answer (its JSON).
    int narrateMaxTokens = 0; // The narrate call's answer (the prose).
    int narrateAllowance = 0; // Resolution record + narrate instruction appended for the narrate call.
    int tailAllowance = 0;    // State view + retrieved snippets + player input + instruction (T1-T4).
    int engineRulesCap = 0;   // L0
    int worldCap = 0;         // L1: world rules + always-on lore.
    int summaryCap = 0;       // L2
    int retrievalCap = 0;     // T2: retrieved lore, history and notes. Part of the tail allowance.
    // Last, so positional initializers of the fields above stay valid.
    int resolveThinkingTokens = 0; // The resolve call's thinking block; 0 turns its thinking off.
    int narrateThinkingTokens = 0; // The narrate call's thinking block; 0 turns its thinking off.
    // The review call: its prompt is standalone and short, so neither is reserved in the resolve prompt's context.
    int reviewMaxTokens = 0;      // The review call's answer (its JSON).
    int reviewThinkingTokens = 0; // The review call's thinking block; 0 turns its thinking off.

    // Generation reserved for each call: its thinking block and its answer.
    int ResolveGenerationTokens() const
    {
        return resolveThinkingTokens + resolveMaxTokens;
    }
    int NarrateGenerationTokens() const
    {
        return narrateThinkingTokens + narrateMaxTokens;
    }
};

// settings.json: per-machine configuration. Not committed; settings.example.json documents it.
struct Settings {
    BackendKind backend = BackendKind::LlamaCpp;
    // llama-cpp only (empty under llama-server): the model files. Relative paths are resolved against the settings
    // file's folder.
    std::filesystem::path chatModel = {};
    std::filesystem::path embeddingModel = {};
    std::string embeddingQueryPrefix = {};     // Prepended to retrieval queries (model-specific).
    std::string embeddingDocumentPrefix = {};  // Prepended to embedded documents (model-specific).
    int contextSize = 0;
    int gpuLayers = 0;                         // llama-cpp only. -1 puts every layer on the GPU.
    // llama-server only (empty under llama-cpp): "http://host:port" of the chat and embedding servers, and the server
    // slot Morpheus's own calls use.
    std::string chatUrl = {};
    std::string embeddingUrl = {};
    int serverSlot = 0;
    BudgetSettings budget = {};
    SamplingSettings resolveSampling = {};
    SamplingSettings narrateSampling = {};
    std::optional<MemoryMode> memoryMode = {}; // Overrides the world's default memory_mode when set.
    // A review call after each narration checks it against the outcome; a contradiction is narrated once more.
    bool narrationCheck = false;
    // The review also writes the turn's digest, and older transcript turns show it instead of their narration.
    bool compactTranscript = false;
    int proseTurns = 2; // With compactTranscript: the newest turns that keep their narration in the transcript (>= 1).
};

// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors.
// `backend` says which keys the models need: under llama-cpp chat_model, embedding_model (both files must exist) and
// gpu_layers; under llama-server a `server` object with chat_url and embedding_url (each "http://" and a host) and
// slot (at least 0). Each backend's keys are ignored, not checked, under the other. The budget must leave room for the
// transcript (L3) and SETTINGS_TEMPLATE_MARGIN chat-template tokens; the review budgets are not part of that sum.
// memory_mode is optional; every other key is required.
Settings LoadSettings(const std::filesystem::path& file);

// The settings as settings.json JSON, with model paths absolute (UTF-8), and only the keys of the backend in use.
// Stored in a run's meta for reference.
nlohmann::json SettingsToJson(const Settings& settings);

} // namespace morpheus::core
