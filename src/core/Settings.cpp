#include "core/Settings.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <initializer_list>
#include <optional>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/JsonInt.h"

namespace morpheus::core {

Settings LoadSettings(const std::filesystem::path& file)
{
    const std::string where = std::format("'{}'", file.generic_string());

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw DataError(std::format("cannot read {}", where));
    }
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(stream);
    } catch (const nlohmann::json::parse_error& error) {
        throw DataError(std::format("{}: {}", where, error.what()));
    }

    // Typos must fail loudly instead of silently falling back to a default.
    auto checkKeys = [&](const nlohmann::json& object, const char* context, std::initializer_list<const char*> allowed) {
        if (!object.is_object()) {
            throw DataError(std::format("{}: {} must be an object", where, context));
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            const bool known = std::any_of(allowed.begin(), allowed.end(), [&](const char* key) { return it.key() == key; });
            if (!known) {
                throw DataError(std::format("{}: unknown key '{}' in {}", where, it.key(), context));
            }
        }
    };
    auto requireMember = [&](const nlohmann::json& object, const char* key, const char* context) -> const nlohmann::json& {
        if (!object.contains(key)) {
            throw DataError(std::format("{}: {} needs '{}'", where, context, key));
        }
        return object.at(key);
    };
    auto requireInt = [&](const nlohmann::json& object, const char* key, const char* context, long long lo, long long hi) {
        const std::optional<long long> value = JsonIntInRange(requireMember(object, key, context), lo, hi);
        if (!value) {
            throw DataError(std::format("{}: {} '{}' must be an integer in {}..{}", where, context, key, lo, hi));
        }
        return static_cast<int>(*value);
    };
    auto requireNumber = [&](const nlohmann::json& object, const char* key, const char* context, double lo, double hi) {
        const nlohmann::json& value = requireMember(object, key, context);
        if (!value.is_number() || value.get<double>() < lo || value.get<double>() > hi) {
            throw DataError(std::format("{}: {} '{}' must be a number in {}..{}", where, context, key, lo, hi));
        }
        return value.get<double>();
    };
    auto requireString = [&](const nlohmann::json& object, const char* key, const char* context) {
        const nlohmann::json& value = requireMember(object, key, context);
        if (!value.is_string()) {
            throw DataError(std::format("{}: {} '{}' must be a string", where, context, key));
        }
        return value.get<std::string>();
    };

    checkKeys(json, "settings", {"chat_model", "embedding_model", "embedding_query_prefix", "embedding_document_prefix",
                                 "context_size", "gpu_layers", "budget", "sampling"});

    Settings settings;

    // Model files: relative to the settings file's folder, and they must exist now rather than at first use.
    auto requireModel = [&](const char* key) {
        const std::string text = requireString(json, key, "settings");
        if (text.empty()) {
            throw DataError(std::format("{}: '{}' must not be empty", where, key));
        }
        // The JSON text is UTF-8; construct the path from char8_t so non-ASCII folders survive on Windows.
        std::filesystem::path path(reinterpret_cast<const char8_t*>(text.c_str()));
        if (path.is_relative()) {
            path = file.parent_path() / path;
        }
        if (!std::filesystem::is_regular_file(path)) {
            throw DataError(std::format("{}: '{}' file '{}' does not exist", where, key, path.generic_string()));
        }
        return path;
    };
    settings.chatModel = requireModel("chat_model");
    settings.embeddingModel = requireModel("embedding_model");
    settings.embeddingQueryPrefix = requireString(json, "embedding_query_prefix", "settings");
    settings.embeddingDocumentPrefix = requireString(json, "embedding_document_prefix", "settings");
    settings.contextSize = requireInt(json, "context_size", "settings", 512, 262144);
    settings.gpuLayers = requireInt(json, "gpu_layers", "settings", -1, 10000);

    // Budget: every part positive and below the context size.
    const nlohmann::json& budget = requireMember(json, "budget", "settings");
    checkKeys(budget, "budget", {"resolve_max_tokens", "narrate_max_tokens", "tail_allowance", "engine_rules_cap",
                                 "world_cap", "summary_cap"});
    const long long contextSize = settings.contextSize;
    settings.budget.resolveMaxTokens = requireInt(budget, "resolve_max_tokens", "budget", 1, contextSize);
    settings.budget.narrateMaxTokens = requireInt(budget, "narrate_max_tokens", "budget", 1, contextSize);
    settings.budget.tailAllowance = requireInt(budget, "tail_allowance", "budget", 1, contextSize);
    settings.budget.engineRulesCap = requireInt(budget, "engine_rules_cap", "budget", 1, contextSize);
    settings.budget.worldCap = requireInt(budget, "world_cap", "budget", 1, contextSize);
    settings.budget.summaryCap = requireInt(budget, "summary_cap", "budget", 1, contextSize);
    const long long fixed = static_cast<long long>(settings.budget.resolveMaxTokens) + settings.budget.narrateMaxTokens +
                            settings.budget.tailAllowance + settings.budget.engineRulesCap + settings.budget.worldCap +
                            settings.budget.summaryCap;
    if (fixed >= contextSize) {
        throw DataError(std::format("{}: budget uses {} of {} context tokens and leaves no room for the transcript",
                                    where, fixed, contextSize));
    }

    // Sampling per call type.
    const nlohmann::json& sampling = requireMember(json, "sampling", "settings");
    checkKeys(sampling, "sampling", {"resolve", "narrate"});
    auto readSampling = [&](const char* callType) {
        const nlohmann::json& object = requireMember(sampling, callType, "sampling");
        checkKeys(object, callType, {"temperature", "top_k", "top_p"});
        SamplingSettings result;
        result.temperature = requireNumber(object, "temperature", callType, 0.0, 2.0);
        result.topK = requireInt(object, "top_k", callType, 1, 1000);
        result.topP = requireNumber(object, "top_p", callType, 0.0, 1.0);
        if (result.topP <= 0.0) {
            throw DataError(std::format("{}: {} 'top_p' must be greater than 0", where, callType));
        }
        return result;
    };
    settings.resolveSampling = readSampling("resolve");
    settings.narrateSampling = readSampling("narrate");
    return settings;
}

} // namespace morpheus::core
