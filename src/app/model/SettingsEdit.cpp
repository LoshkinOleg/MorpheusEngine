#include "app/model/SettingsEdit.h"

#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/Text.h"

namespace morpheus::app {

namespace {

// The settings file as JSON, keeping its key order. Throws DataError naming the file when it cannot be read or
// parsed.
nlohmann::ordered_json ReadJson(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw core::DataError(std::format("cannot read {}", core::PathText(file)));
    }
    try {
        return nlohmann::ordered_json::parse(stream);
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("{}: {}", core::PathText(file), error.what()));
    }
}

} // namespace

SettingsEdit ReadSettingsEdit(const std::filesystem::path& file)
{
    const nlohmann::ordered_json json = ReadJson(file);
    try {
        // One call type's sampling block.
        auto sampling = [](const nlohmann::ordered_json& block) {
            return core::SamplingSettings{block.at("temperature").get<double>(), block.at("top_k").get<int>(),
                                          block.at("top_p").get<double>()};
        };
        SettingsEdit edit;
        const std::string backendText = json.at("backend").get<std::string>();
        const std::optional<core::BackendKind> backend = core::ParseBackendKind(backendText);
        if (!backend) {
            throw core::DataError(
                std::format("{}: 'backend' must be 'llama-cpp' or 'llama-server'", core::PathText(file)));
        }
        edit.backend = *backend;
        // The backend in use needs its keys; the other backend's are read when the file has them, so a switch in the
        // window starts from them.
        const bool local = edit.backend == core::BackendKind::LlamaCpp;
        if (local || json.contains("chat_model")) {
            edit.chatModel = json.at("chat_model").get<std::string>();
        }
        if (local || json.contains("embedding_model")) {
            edit.embeddingModel = json.at("embedding_model").get<std::string>();
        }
        if (local || json.contains("gpu_layers")) {
            edit.gpuLayers = json.at("gpu_layers").get<int>();
        }
        if (!local || json.contains("server")) {
            const nlohmann::ordered_json& server = json.at("server");
            if (!local || server.contains("chat_url")) {
                edit.chatUrl = server.at("chat_url").get<std::string>();
            }
            if (!local || server.contains("embedding_url")) {
                edit.embeddingUrl = server.at("embedding_url").get<std::string>();
            }
            if (!local || server.contains("slot")) {
                edit.serverSlot = server.at("slot").get<int>();
            }
        }
        edit.contextSize = json.at("context_size").get<int>();
        edit.resolveThinkingTokens = json.at("budget").at("resolve_thinking_tokens").get<int>();
        edit.narrateThinkingTokens = json.at("budget").at("narrate_thinking_tokens").get<int>();
        edit.reviewThinkingTokens = json.at("budget").at("review_thinking_tokens").get<int>();
        edit.resolveSampling = sampling(json.at("sampling").at("resolve"));
        edit.narrateSampling = sampling(json.at("sampling").at("narrate"));
        edit.narrationCheck = json.at("narration_check").get<bool>();
        edit.compactTranscript = json.at("compact_transcript").get<bool>();
        edit.proseTurns = json.at("prose_turns").get<int>();
        if (json.contains("memory_mode")) {
            edit.memoryMode = core::ParseMemoryMode(json.at("memory_mode").get<std::string>());
            if (!edit.memoryMode) {
                throw core::DataError(
                    std::format("{}: 'memory_mode' must be 'engine' or 'assisted'", core::PathText(file)));
            }
        }
        return edit;
    } catch (const nlohmann::json::exception& error) {
        // A missing key or a value of the wrong type.
        throw core::DataError(std::format("{}: {}", core::PathText(file), error.what()));
    }
}

core::Settings SaveSettingsEdit(const std::filesystem::path& file, const SettingsEdit& edit)
{
    // Patch the file's own JSON, so keys the window does not edit, their order and relative paths stay as written.
    nlohmann::ordered_json json = ReadJson(file);
    auto samplingJson = [](const core::SamplingSettings& sampling) {
        return nlohmann::ordered_json{
            {"temperature", sampling.temperature}, {"top_k", sampling.topK}, {"top_p", sampling.topP}};
    };
    try {
        // The backend and its own keys; the other backend's keys stay as the file has them.
        json["backend"] = core::BackendKindName(edit.backend);
        if (edit.backend == core::BackendKind::LlamaCpp) {
            json["chat_model"] = edit.chatModel;
            json["embedding_model"] = edit.embeddingModel;
            json["gpu_layers"] = edit.gpuLayers;
        } else {
            json["server"]["chat_url"] = edit.chatUrl;
            json["server"]["embedding_url"] = edit.embeddingUrl;
            json["server"]["slot"] = edit.serverSlot;
        }
        json["context_size"] = edit.contextSize;
        json["budget"]["resolve_thinking_tokens"] = edit.resolveThinkingTokens;
        json["budget"]["narrate_thinking_tokens"] = edit.narrateThinkingTokens;
        json["budget"]["review_thinking_tokens"] = edit.reviewThinkingTokens;
        json["sampling"]["resolve"] = samplingJson(edit.resolveSampling);
        json["sampling"]["narrate"] = samplingJson(edit.narrateSampling);
        json["narration_check"] = edit.narrationCheck;
        json["compact_transcript"] = edit.compactTranscript;
        json["prose_turns"] = edit.proseTurns;
        if (edit.memoryMode) {
            json["memory_mode"] = *edit.memoryMode == core::MemoryMode::Engine ? "engine" : "assisted";
        } else {
            json.erase("memory_mode");
        }
    } catch (const nlohmann::json::exception& error) {
        // Valid JSON of the wrong shape, such as "sampling": [] or a top-level array.
        throw core::DataError(std::format("{}: {}", core::PathText(file), error.what()));
    }

    // Check the result with the real loader before replacing anything. The check file sits beside the settings
    // file, so relative model paths resolve the same way.
    std::filesystem::path checkFile = file;
    checkFile += ".new";
    try {
        {
            std::ofstream stream(checkFile, std::ios::binary | std::ios::trunc);
            stream << json.dump(2) << '\n';
            stream.close();
            if (stream.fail()) {
                throw core::DataError(std::format("cannot write {}", core::PathText(checkFile)));
            }
        }
        core::Settings settings;
        try {
            settings = core::LoadSettings(checkFile);
        } catch (const core::DataError& error) {
            // The loader names the check file; the player knows only the settings file.
            std::string message = error.what();
            const std::string checkText = core::PathText(checkFile);
            for (std::size_t at = message.find(checkText); at != std::string::npos; at = message.find(checkText, at)) {
                message.replace(at, checkText.size(), core::PathText(file));
            }
            throw core::DataError(message);
        }
        std::error_code renameError;
        std::filesystem::rename(checkFile, file, renameError); // Replaces the settings file in one step.
        if (renameError) {
            throw core::DataError(std::format("cannot replace {}: {}", core::PathText(file), renameError.message()));
        }
        return settings;
    } catch (...) {
        // Whatever failed, the settings file is as it was and no check file stays behind. A check file that will not
        // delete must not replace the error the player needs.
        std::error_code ignored;
        std::filesystem::remove(checkFile, ignored);
        throw;
    }
}

} // namespace morpheus::app
