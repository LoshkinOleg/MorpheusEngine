#include "app/model/SettingsEdit.h"

#include <cstddef>
#include <format>
#include <fstream>
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
        edit.chatModel = json.at("chat_model").get<std::string>();
        edit.embeddingModel = json.at("embedding_model").get<std::string>();
        edit.contextSize = json.at("context_size").get<int>();
        edit.gpuLayers = json.at("gpu_layers").get<int>();
        edit.resolveSampling = sampling(json.at("sampling").at("resolve"));
        edit.narrateSampling = sampling(json.at("sampling").at("narrate"));
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
        json["chat_model"] = edit.chatModel;
        json["embedding_model"] = edit.embeddingModel;
        json["context_size"] = edit.contextSize;
        json["gpu_layers"] = edit.gpuLayers;
        json["sampling"]["resolve"] = samplingJson(edit.resolveSampling);
        json["sampling"]["narrate"] = samplingJson(edit.narrateSampling);
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
