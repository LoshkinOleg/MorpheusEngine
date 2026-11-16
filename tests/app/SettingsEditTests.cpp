#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
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
    CHECK(edit.backend == core::BackendKind::LlamaCpp); // As settings.example.json sets it.
    CHECK(edit.chatUrl.empty());                        // The example has no server object.
    CHECK(edit.chatModel == file.at("chat_model").get<std::string>()); // As written, not resolved.
    CHECK(edit.embeddingModel == file.at("embedding_model").get<std::string>());
    CHECK(edit.contextSize == file.at("context_size").get<int>());
    CHECK(edit.gpuLayers == file.at("gpu_layers").get<int>());
    CHECK(edit.resolveThinkingTokens == file.at("budget").at("resolve_thinking_tokens").get<int>());
    CHECK(edit.narrateThinkingTokens == file.at("budget").at("narrate_thinking_tokens").get<int>());
    CHECK(edit.reviewThinkingTokens == file.at("budget").at("review_thinking_tokens").get<int>());
    CHECK(edit.narrationCheck == file.at("narration_check").get<bool>());
    CHECK(edit.compactTranscript == file.at("compact_transcript").get<bool>());
    CHECK(edit.proseTurns == file.at("prose_turns").get<int>());
    CHECK(edit.resolveSampling.temperature == file.at("sampling").at("resolve").at("temperature").get<double>());
    CHECK(edit.narrateSampling.topK == file.at("sampling").at("narrate").at("top_k").get<int>());
    CHECK(edit.narrateSampling.topP == file.at("sampling").at("narrate").at("top_p").get<double>());
    CHECK(edit.narrateSampling.minP == 0.0);
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
    edit.resolveThinkingTokens = 0; // A model without a thinking mode.
    edit.narrateThinkingTokens = 0;

    const core::Settings saved = app::SaveSettingsEdit(file, edit);
    CHECK(saved.narrateSampling.temperature == 1.1);
    CHECK(saved.budget.resolveThinkingTokens == 0);
    CHECK(saved.budget.narrateThinkingTokens == 0);
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

    // The loader's reason reaches the player, and names the real file rather than the check file.
    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("does not exist"));
    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("settings.json"));
    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), !ContainsSubstring("settings.json.new"));
    CHECK(test::ReadFile(file) == before);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
}

TEST_CASE("SaveSettingsEdit reports a misshapen settings file as a DataError", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_misshapen");
    const std::filesystem::path file = dir / "settings.json";
    const app::SettingsEdit edit = app::ReadSettingsEdit(file);

    // Valid JSON that is not the settings shape: the patch itself fails, not the parse.
    nlohmann::json badSampling = nlohmann::json::parse(test::ReadFile(file));
    badSampling["sampling"] = nlohmann::json::array();
    for (const nlohmann::json& misshapen : {badSampling, nlohmann::json::array({1, 2})}) {
        test::WriteFile(file, misshapen.dump(2));
        const std::string before = test::ReadFile(file);
        CHECK_THROWS_AS(app::SaveSettingsEdit(file, edit), core::DataError);
        CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("settings.json"));
        CHECK(test::ReadFile(file) == before);
        CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
    }
}

// Windows only: the test needs a replace that fails, and only Windows refuses to replace a file that another handle
// holds open without delete sharing. On Linux the rename replaces the file regardless, so the save succeeds and there
// is no failure to report.
#ifdef _WIN32
TEST_CASE("SaveSettingsEdit reports a file it cannot replace and leaves no check file", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_unreplaceable");
    const std::filesystem::path file = dir / "settings.json";
    const app::SettingsEdit edit = app::ReadSettingsEdit(file);

    // A reader that holds the file open without delete sharing makes Windows refuse to replace it.
    const std::string before = test::ReadFile(file);
    std::ifstream holder(file, std::ios::binary);
    REQUIRE(holder.is_open());
    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("cannot replace"));
    CHECK_THROWS_AS(app::SaveSettingsEdit(file, edit), core::DataError);
    holder.close();
    CHECK(test::ReadFile(file) == before);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
}
#endif

TEST_CASE("ReadSettingsEdit names the file when it cannot be read", "[settings_edit]")
{
    const std::filesystem::path dir = test::FreshTempDir("settings_edit_unreadable");
    test::WriteFile(dir / "settings.json", "{ not json");
    CHECK_THROWS_WITH(app::ReadSettingsEdit(dir / "settings.json"), ContainsSubstring("settings.json"));
    CHECK_THROWS_AS(app::ReadSettingsEdit(dir / "missing.json"), core::DataError);
}

TEST_CASE("SaveSettingsEdit writes the review settings and refuses prose_turns 0", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_review");
    const std::filesystem::path file = dir / "settings.json";
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.narrationCheck = false;
    edit.compactTranscript = false;
    edit.proseTurns = 3;
    edit.reviewThinkingTokens = 128;

    const core::Settings saved = app::SaveSettingsEdit(file, edit);
    CHECK_FALSE(saved.narrationCheck);
    CHECK_FALSE(saved.compactTranscript);
    CHECK(saved.proseTurns == 3);
    CHECK(saved.budget.reviewThinkingTokens == 128);
    CHECK(saved.budget.reviewMaxTokens == 384); // Not edited: kept.

    // The loader's rule reaches the player, and the file stays as it was.
    const std::string before = test::ReadFile(file);
    edit.proseTurns = 0;
    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("prose_turns"));
    CHECK(test::ReadFile(file) == before);
}

TEST_CASE("SaveSettingsEdit switches the backend to llama-server and back, keeping the other backend's keys",
          "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_server");
    const std::filesystem::path file = dir / "settings.json";
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    const std::string chatModel = edit.chatModel;

    edit.backend = core::BackendKind::LlamaServer;
    edit.chatUrl = "http://127.0.0.1:8080";
    edit.embeddingUrl = "http://127.0.0.1:8081";
    edit.serverSlot = 2;
    const core::Settings server = app::SaveSettingsEdit(file, edit);
    CHECK(server.backend == core::BackendKind::LlamaServer);
    CHECK(server.chatUrl == "http://127.0.0.1:8080");
    CHECK(server.embeddingUrl == "http://127.0.0.1:8081");
    CHECK(server.serverSlot == 2);
    const nlohmann::json written = nlohmann::json::parse(test::ReadFile(file));
    CHECK(written.at("backend") == "llama-server");
    CHECK(written.at("server").at("slot") == 2);
    CHECK(written.at("chat_model") == chatModel); // The in-process backend's keys stay, ignored.

    // Read back, then switched back: the server keys stay in the file for the next switch.
    app::SettingsEdit reread = app::ReadSettingsEdit(file);
    CHECK(reread.backend == core::BackendKind::LlamaServer);
    CHECK(reread.chatUrl == "http://127.0.0.1:8080");
    CHECK(reread.serverSlot == 2);
    CHECK(reread.chatModel == chatModel);
    reread.backend = core::BackendKind::LlamaCpp;
    const core::Settings local = app::SaveSettingsEdit(file, reread);
    CHECK(local.backend == core::BackendKind::LlamaCpp);
    CHECK(nlohmann::json::parse(test::ReadFile(file)).at("server").at("chat_url") == "http://127.0.0.1:8080");
    CHECK(app::ReadSettingsEdit(file).embeddingUrl == "http://127.0.0.1:8081");
}

TEST_CASE("SaveSettingsEdit refuses a llama-server edit without its URLs", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_server_invalid");
    const std::filesystem::path file = dir / "settings.json";
    const std::string before = test::ReadFile(file);
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.backend = core::BackendKind::LlamaServer;
    edit.embeddingUrl = "http://127.0.0.1:8081"; // chat_url stays empty.

    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("chat_url"));
    CHECK(test::ReadFile(file) == before);
}

TEST_CASE("ReadSettingsEdit reads a llama-server file without model keys", "[settings_edit]")
{
    const std::filesystem::path dir = test::FreshTempDir("settings_edit_server_only");
    nlohmann::json json = nlohmann::json::parse(test::ReadFile(test::RepoDir() / "settings.example.json"));
    json.erase("chat_model");
    json.erase("embedding_model");
    json.erase("gpu_layers");
    json["backend"] = "llama-server";
    json["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 1}};
    test::WriteFile(dir / "settings.json", json.dump(2));

    const app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    CHECK(edit.backend == core::BackendKind::LlamaServer);
    CHECK(edit.chatModel.empty());
    CHECK(edit.serverSlot == 1);
    // Saving it writes no model keys.
    const core::Settings saved = app::SaveSettingsEdit(dir / "settings.json", edit);
    CHECK(saved.backend == core::BackendKind::LlamaServer);
    CHECK_FALSE(nlohmann::json::parse(test::ReadFile(dir / "settings.json")).contains("chat_model"));
}

TEST_CASE("SaveSettingsEdit writes min_p", "[settings_edit]")
{
    const std::filesystem::path file = test::SettingsFolder("settings_edit_min_p") / "settings.json";
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.resolveSampling.minP = 0.1;
    app::SaveSettingsEdit(file, edit);
    CHECK(nlohmann::json::parse(test::ReadFile(file)).at("sampling").at("resolve").at("min_p").get<double>() == 0.1);
    CHECK(core::LoadSettings(file).resolveSampling.minP == 0.1);
}

TEST_CASE("SaveSettingsEdit keeps thinking_format, which the editor does not show", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_thinking_format");
    const std::filesystem::path file = dir / "settings.json";
    nlohmann::json json = nlohmann::json::parse(test::ReadFile(file));
    json["thinking_format"] = "qwen";
    test::WriteFile(file, json.dump(2));

    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.narrateSampling.temperature = 1.1; // Any edit: the file is rewritten.
    const core::Settings saved = app::SaveSettingsEdit(file, edit);

    // The key is still in the file as written, and the saved settings still use the preset.
    CHECK(nlohmann::json::parse(test::ReadFile(file)).at("thinking_format") == "qwen");
    CHECK(saved.thinking.mode == core::ThinkingMode::Format);
    CHECK(core::LoadSettings(file).thinking == saved.thinking);
}
