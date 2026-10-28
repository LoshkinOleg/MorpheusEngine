#pragma once

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "app/model/AppController.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"

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

// Settings for engines on a ScriptedBackend: the budgets the worker tests use, and model paths that are never opened.
inline core::Settings EngineSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf";
    settings.embeddingModel = "models/embed.gguf";
    settings.contextSize = 16384;
    settings.budget = {256, 256, 256, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

// App paths for a test: the settings in `dir`, the test worlds, and saves in `dir`.
inline app::AppPaths PathsIn(const std::filesystem::path& dir)
{
    return {dir / "settings.json", TestDataDir() / "worlds", dir / "saves"};
}

// A factory for ScriptedBackends that answer with `responder`, sized by the settings' context. A piece delay slows
// streaming, so a test can catch a turn midway.
inline app::EngineHost::BackendFactory ScriptedFactory(ScriptedBackend::Responder responder,
                                                       std::chrono::milliseconds pieceDelay = {})
{
    return [responder, pieceDelay](const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        auto pBackend = std::make_unique<ScriptedBackend>(settings.contextSize, responder);
        pBackend->SetPieceDelay(pieceDelay);
        return pBackend;
    };
}

// Runs the controller's frames until `done` holds. Fails the test after 10 s.
template <typename Done>
void PumpUntil(app::AppController& controller, Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        controller.Frame();
        if (done()) {
            return;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace morpheus::test
