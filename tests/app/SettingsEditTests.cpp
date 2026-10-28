#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "app/AppTestSupport.h"
#include "app/model/SettingsEdit.h"
#include "core/Errors.h"
#include "core/Settings.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("ReadSettingsEdit reads the edited values as the file writes them", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_read");
    const nlohmann::json file = nlohmann::json::parse(test::ReadFile(dir / "settings.json"));
    const app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    CHECK(edit.chatModel == file.at("chat_model").get<std::string>()); // As written, not resolved.
    CHECK(edit.embeddingModel == file.at("embedding_model").get<std::string>());
    CHECK(edit.contextSize == file.at("context_size").get<int>());
    CHECK(edit.gpuLayers == file.at("gpu_layers").get<int>());
    CHECK(edit.resolveSampling.temperature == file.at("sampling").at("resolve").at("temperature").get<double>());
    CHECK(edit.narrateSampling.topK == file.at("sampling").at("narrate").at("top_k").get<int>());
    CHECK(edit.narrateSampling.topP == file.at("sampling").at("narrate").at("top_p").get<double>());
    CHECK_FALSE(edit.memoryMode); // The example leaves memory_mode to each world.
}

TEST_CASE("SaveSettingsEdit writes the edit and keeps everything else", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_save");
    const std::filesystem::path file = dir / "settings.json";
    const core::Settings before = core::LoadSettings(file);
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.narrateSampling.temperature = 1.1;
    edit.memoryMode = core::MemoryMode::Assisted;

    const core::Settings saved = app::SaveSettingsEdit(file, edit);
    CHECK(saved.narrateSampling.temperature == 1.1);
    CHECK(saved.memoryMode.value_or(core::MemoryMode::Engine) == core::MemoryMode::Assisted);

    const core::Settings reloaded = core::LoadSettings(file);
    CHECK(reloaded.narrateSampling.temperature == 1.1);
    CHECK(reloaded.budget.retrievalCap == before.budget.retrievalCap);           // Not edited: kept.
    CHECK(reloaded.embeddingQueryPrefix == before.embeddingQueryPrefix);         // Not edited: kept.
    CHECK(app::ReadSettingsEdit(file).chatModel == edit.chatModel);              // Still as written.
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));

    edit.memoryMode.reset(); // Back to each world's default: the key goes away.
    app::SaveSettingsEdit(file, edit);
    CHECK_FALSE(core::LoadSettings(file).memoryMode);
    CHECK_FALSE(nlohmann::json::parse(test::ReadFile(file)).contains("memory_mode"));
}

TEST_CASE("SaveSettingsEdit leaves the file untouched when the edit is invalid", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_invalid");
    const std::filesystem::path file = dir / "settings.json";
    const std::string before = test::ReadFile(file);
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.chatModel = "models/missing.gguf";

    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("does not exist"));
    CHECK(test::ReadFile(file) == before);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
}

TEST_CASE("ReadSettingsEdit names the file when it cannot be read", "[settings_edit]")
{
    const std::filesystem::path dir = test::FreshTempDir("settings_edit_unreadable");
    test::WriteFile(dir / "settings.json", "{ not json");
    CHECK_THROWS_WITH(app::ReadSettingsEdit(dir / "settings.json"), ContainsSubstring("settings.json"));
    CHECK_THROWS_AS(app::ReadSettingsEdit(dir / "missing.json"), core::DataError);
}
