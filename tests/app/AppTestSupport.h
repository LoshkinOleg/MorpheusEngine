#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "TestPaths.h"

namespace morpheus::test {

// The whole file as bytes. Throws when it cannot be read.
inline std::string ReadFile(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("ReadFile: cannot read '" + file.generic_string() + "'");
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// A folder laid out like the repository root for settings: settings.example.json copied to settings.json, and an
// empty file at each model path it names, since core::LoadSettings requires the model files to exist.
inline std::filesystem::path SettingsFolder(std::string_view name)
{
    const std::filesystem::path dir = FreshTempDir(name);
    std::filesystem::copy_file(RepoDir() / "settings.example.json", dir / "settings.json");
    const nlohmann::json settings = nlohmann::json::parse(ReadFile(dir / "settings.json"));
    for (const char* key : {"chat_model", "embedding_model"}) {
        const std::filesystem::path model = dir / settings.at(key).get<std::string>();
        std::filesystem::create_directories(model.parent_path());
        WriteFile(model, "");
    }
    return dir;
}

} // namespace morpheus::test
