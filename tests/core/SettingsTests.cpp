#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/Settings.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

// A valid settings document whose model paths are relative to the settings file's folder.
nlohmann::json ValidSettingsJson()
{
    return nlohmann::json::parse(R"({
        "chat_model": "models/chat.gguf",
        "embedding_model": "models/embed.gguf",
        "embedding_query_prefix": "search_query: ",
        "embedding_document_prefix": "search_document: ",
        "context_size": 16384,
        "gpu_layers": -1,
        "budget": {
            "resolve_max_tokens": 512, "narrate_max_tokens": 768, "narrate_allowance": 512, "tail_allowance": 2048,
            "engine_rules_cap": 1024, "world_cap": 4096, "summary_cap": 2048
        },
        "sampling": {
            "resolve": {"temperature": 0.3, "top_k": 40, "top_p": 0.9},
            "narrate": {"temperature": 0.8, "top_k": 40, "top_p": 0.95}
        }
    })");
}

// Writes `json` as settings.json into a fresh folder that also holds the two dummy model files.
fs::path WriteSettings(const std::string& testName, const nlohmann::json& json)
{
    const fs::path dir = morpheus::test::FreshTempDir(testName);
    fs::create_directories(dir / "models");
    morpheus::test::WriteFile(dir / "models" / "chat.gguf", "x");
    morpheus::test::WriteFile(dir / "models" / "embed.gguf", "x");
    morpheus::test::WriteFile(dir / "settings.json", json.dump(2));
    return dir / "settings.json";
}

} // namespace

TEST_CASE("LoadSettings reads every value and resolves model paths", "[settings]")
{
    const fs::path file = WriteSettings("settings_valid", ValidSettingsJson());
    const Settings settings = LoadSettings(file);

    CHECK(settings.chatModel == file.parent_path() / "models" / "chat.gguf");
    CHECK(settings.embeddingModel == file.parent_path() / "models" / "embed.gguf");
    CHECK(settings.embeddingQueryPrefix == "search_query: ");
    CHECK(settings.contextSize == 16384);
    CHECK(settings.gpuLayers == -1);
    CHECK(settings.budget.narrateMaxTokens == 768);
    CHECK(settings.budget.narrateAllowance == 512);
    CHECK(settings.budget.summaryCap == 2048);
    CHECK(settings.resolveSampling.temperature == 0.3);
    CHECK(settings.narrateSampling.topP == 0.95);
}

TEST_CASE("LoadSettings names what is wrong", "[settings]")
{
    SECTION("missing model file")
    {
        nlohmann::json json = ValidSettingsJson();
        json["chat_model"] = "models/missing.gguf";
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_missing_model", json)), ContainsSubstring("chat_model"));
    }
    SECTION("unknown key")
    {
        nlohmann::json json = ValidSettingsJson();
        json["contxt_size"] = 1;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_unknown_key", json)), ContainsSubstring("contxt_size"));
    }
    SECTION("unknown nested key")
    {
        nlohmann::json json = ValidSettingsJson();
        json["sampling"]["narrate"]["temprature"] = 1.0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_unknown_nested", json)), ContainsSubstring("temprature"));
    }
    SECTION("budget leaves no room for the transcript")
    {
        nlohmann::json json = ValidSettingsJson();
        json["context_size"] = 8192;
        json["budget"]["world_cap"] = 6000;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_budget", json)), ContainsSubstring("transcript"));
    }
    SECTION("narrate_allowance is required")
    {
        nlohmann::json json = ValidSettingsJson();
        json["budget"].erase("narrate_allowance");
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_no_narrate_allowance", json)),
                          ContainsSubstring("narrate_allowance"));
    }
    SECTION("narrate_allowance above the context size")
    {
        nlohmann::json json = ValidSettingsJson();
        json["budget"]["narrate_allowance"] = 20000;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_big_narrate_allowance", json)),
                          ContainsSubstring("narrate_allowance"));
    }
    SECTION("narrate_allowance counts toward the transcript room")
    {
        // 512 + 768 + 2048 + 1024 + 4096 + 2048 = 10496 fits 16384; adding 6000 does not.
        nlohmann::json json = ValidSettingsJson();
        json["budget"]["narrate_allowance"] = 6000;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_allowance_budget", json)), ContainsSubstring("transcript"));
    }
    SECTION("top_p of zero")
    {
        nlohmann::json json = ValidSettingsJson();
        json["sampling"]["resolve"]["top_p"] = 0.0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_top_p", json)), ContainsSubstring("top_p"));
    }
    SECTION("temperature out of range")
    {
        nlohmann::json json = ValidSettingsJson();
        json["sampling"]["narrate"]["temperature"] = 3.0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_temperature", json)), ContainsSubstring("temperature"));
    }
}
