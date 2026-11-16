#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

// A valid settings document whose model paths are relative to the settings file's folder.
nlohmann::json ValidSettingsJson()
{
    return nlohmann::json::parse(R"({
        "backend": "llama-cpp",
        "chat_model": "models/chat.gguf",
        "embedding_model": "models/embed.gguf",
        "embedding_query_prefix": "search_query: ",
        "embedding_document_prefix": "search_document: ",
        "context_size": 16384,
        "gpu_layers": -1,
        "narration_check": true,
        "compact_transcript": true,
        "prose_turns": 2,
        "budget": {
            "resolve_max_tokens": 512, "narrate_max_tokens": 768, "review_max_tokens": 384,
            "resolve_thinking_tokens": 0, "narrate_thinking_tokens": 0, "review_thinking_tokens": 0,
            "narrate_allowance": 512, "tail_allowance": 2048,
            "engine_rules_cap": 1024, "world_cap": 4096, "summary_cap": 2048, "retrieval_cap": 768
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
    SECTION("missing required key")
    {
        nlohmann::json json = ValidSettingsJson();
        json.erase("context_size");
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_missing_key", json)), ContainsSubstring("context_size"));
    }
    SECTION("wrong type")
    {
        nlohmann::json json = ValidSettingsJson();
        json["gpu_layers"] = "all";
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_wrong_type", json)), ContainsSubstring("gpu_layers"));
    }
    SECTION("malformed JSON")
    {
        const fs::path file = WriteSettings("settings_malformed", ValidSettingsJson());
        morpheus::test::WriteFile(file, "{");
        CHECK_THROWS_WITH(LoadSettings(file), ContainsSubstring("settings.json"));
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

TEST_CASE("LoadSettings reads retrieval_cap and an optional memory_mode", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    const Settings plain = LoadSettings(WriteSettings("settings_retrieval", json));
    CHECK(plain.budget.retrievalCap == 768);
    CHECK_FALSE(plain.memoryMode.has_value());

    json["memory_mode"] = "assisted";
    CHECK(LoadSettings(WriteSettings("settings_memory_mode", json)).memoryMode == MemoryMode::Assisted);

    json["memory_mode"] = "telepathic";
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_memory_mode_bad", json)), ContainsSubstring("memory_mode"));

    json["memory_mode"] = "engine";
    json["budget"]["retrieval_cap"] = 4096; // Over tail_allowance (2048).
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_retrieval_big", json)), ContainsSubstring("retrieval_cap"));

    json["budget"].erase("retrieval_cap");
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_retrieval_missing", json)), ContainsSubstring("retrieval_cap"));
}

TEST_CASE("LoadSettings reads the thinking budgets", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["budget"]["resolve_thinking_tokens"] = 2048;
    json["budget"]["narrate_thinking_tokens"] = MIN_THINKING_TOKENS;
    const Settings settings = LoadSettings(WriteSettings("settings_thinking", json));
    CHECK(settings.budget.resolveThinkingTokens == 2048);
    CHECK(settings.budget.narrateThinkingTokens == MIN_THINKING_TOKENS);
    CHECK(settings.budget.ResolveGenerationTokens() == 2048 + 512);
    CHECK(settings.budget.NarrateGenerationTokens() == MIN_THINKING_TOKENS + 768);

    SECTION("zero turns a call's thinking off")
    {
        json["budget"]["narrate_thinking_tokens"] = 0;
        CHECK(LoadSettings(WriteSettings("settings_thinking_off", json)).budget.NarrateGenerationTokens() == 768);
    }
    SECTION("each budget is required")
    {
        json["budget"].erase("narrate_thinking_tokens");
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_thinking_missing", json)),
                          ContainsSubstring("narrate_thinking_tokens"));
    }
    SECTION("a negative budget")
    {
        json["budget"]["resolve_thinking_tokens"] = -1;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_thinking_negative", json)),
                          ContainsSubstring("resolve_thinking_tokens"));
    }
    SECTION("a budget too small to close the block")
    {
        json["budget"]["resolve_thinking_tokens"] = MIN_THINKING_TOKENS - 1;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_thinking_small", json)),
                          ContainsSubstring("resolve_thinking_tokens") && ContainsSubstring("at least 64"));
    }
    SECTION("thinking counts toward the transcript room")
    {
        // The other parts use 11008 of 16384 tokens; 2048 + 64 thinking still fits, 4096 more does not.
        json["budget"]["narrate_thinking_tokens"] = 4096;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_thinking_room", json)), ContainsSubstring("transcript"));
    }
}

TEST_CASE("LoadSettings keeps a margin for chat-template tokens", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    // The budget parts (retrieval is inside the tail) sum to 11008; 30 tokens more leaves no room for template
    // tokens and a transcript.
    json["context_size"] = 11008 + 30;
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_margin", json)), ContainsSubstring("no room for the transcript"));
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_margin_text", json)),
                      ContainsSubstring("11008 of 11038 context tokens (plus a 64-token chat-template margin)"));
}

TEST_CASE("LoadSettings reads the narration review and compact transcript settings", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["narration_check"] = false;
    json["prose_turns"] = 3;
    json["budget"]["review_thinking_tokens"] = MIN_THINKING_TOKENS;
    const Settings settings = LoadSettings(WriteSettings("settings_review", json));
    CHECK_FALSE(settings.narrationCheck);
    CHECK(settings.compactTranscript);
    CHECK(settings.proseTurns == 3);
    CHECK(settings.budget.reviewMaxTokens == 384);
    CHECK(settings.budget.reviewThinkingTokens == MIN_THINKING_TOKENS);

    SECTION("each key is required")
    {
        for (const std::string key : {"narration_check", "compact_transcript", "prose_turns"}) {
            nlohmann::json missing = ValidSettingsJson();
            missing.erase(key);
            CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_missing_" + key, missing)),
                              ContainsSubstring(key));
        }
        for (const std::string key : {"review_max_tokens", "review_thinking_tokens"}) {
            nlohmann::json missing = ValidSettingsJson();
            missing["budget"].erase(key);
            CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_missing_" + key, missing)),
                              ContainsSubstring(key));
        }
    }
    SECTION("prose_turns is at least 1")
    {
        json["prose_turns"] = 0;
        CHECK_THROWS_AS(LoadSettings(WriteSettings("settings_prose_turns_zero", json)), DataError);
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_prose_turns_zero", json)), ContainsSubstring("prose_turns"));
    }
    SECTION("review_thinking_tokens is 0 or at least the minimum")
    {
        json["budget"]["review_thinking_tokens"] = 10;
        CHECK_THROWS_AS(LoadSettings(WriteSettings("settings_review_thinking_small", json)), DataError);
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_thinking_small", json)),
                          ContainsSubstring("review_thinking_tokens") && ContainsSubstring("at least 64"));
    }
    SECTION("review_max_tokens is positive")
    {
        json["budget"]["review_max_tokens"] = 0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_max_zero", json)),
                          ContainsSubstring("review_max_tokens"));
    }
    SECTION("the review answer and thinking must leave room in the context")
    {
        json["budget"]["review_max_tokens"] = 16384 - MIN_THINKING_TOKENS;
        json["budget"]["review_thinking_tokens"] = MIN_THINKING_TOKENS;
        CHECK_THROWS_AS(LoadSettings(WriteSettings("settings_review_fit", json)), DataError);
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_fit", json)),
                          ContainsSubstring("review_max_tokens") && ContainsSubstring("review_thinking_tokens"));
    }
    SECTION("the switches are true or false")
    {
        json["narration_check"] = "yes";
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_switch", json)),
                          ContainsSubstring("narration_check") && ContainsSubstring("true or false"));
    }
}

TEST_CASE("SettingsToJson round-trips through LoadSettings", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["memory_mode"] = "assisted";
    json["budget"]["resolve_thinking_tokens"] = 128;
    json["budget"]["narrate_thinking_tokens"] = 256;
    json["narration_check"] = false;
    json["prose_turns"] = 5;
    json["budget"]["review_max_tokens"] = 200;
    json["budget"]["review_thinking_tokens"] = 64;
    const Settings original = LoadSettings(WriteSettings("settings_to_json_a", json));
    // Model paths in the JSON are absolute, so they still resolve from another folder.
    const Settings copy = LoadSettings(WriteSettings("settings_to_json_b", SettingsToJson(original)));
    CHECK(copy.chatModel == original.chatModel);
    CHECK(copy.backend == BackendKind::LlamaCpp);
    CHECK(copy.embeddingDocumentPrefix == original.embeddingDocumentPrefix);
    CHECK(copy.contextSize == original.contextSize);
    CHECK(copy.budget.retrievalCap == original.budget.retrievalCap);
    CHECK(copy.budget.narrateAllowance == original.budget.narrateAllowance);
    CHECK(copy.budget.resolveThinkingTokens == original.budget.resolveThinkingTokens);
    CHECK(copy.budget.narrateThinkingTokens == original.budget.narrateThinkingTokens);
    CHECK(copy.narrateSampling.topP == original.narrateSampling.topP);
    CHECK(copy.memoryMode == MemoryMode::Assisted);
    CHECK(copy.narrationCheck == original.narrationCheck);
    CHECK(copy.compactTranscript == original.compactTranscript);
    CHECK(copy.proseTurns == 5);
    CHECK(copy.budget.reviewMaxTokens == 200);
    CHECK(copy.budget.reviewThinkingTokens == 64);
}

TEST_CASE("LoadSettings makes model paths absolute for a relative settings path", "[settings]")
{
    // Restores the working directory when the test leaves scope, even if a check fails.
    struct CurrentPathGuard {
        fs::path saved = fs::current_path();
        ~CurrentPathGuard() { fs::current_path(saved); }
    };

    const fs::path file = WriteSettings("settings_relative_path", ValidSettingsJson());
    const fs::path folder = file.parent_path();
    // Load through "<folder name>/settings.json" from the folder's parent, so the file's parent path is relative.
    const fs::path relativeFile = folder.filename() / "settings.json";
    CurrentPathGuard guard;
    fs::current_path(folder.parent_path());

    const Settings settings = LoadSettings(relativeFile);
    CHECK(settings.chatModel.is_absolute());
    CHECK(settings.embeddingModel.is_absolute());
    CHECK(fs::equivalent(settings.chatModel, folder / "models" / "chat.gguf"));
    // The stored form is what run meta records, so it must also be absolute.
    const fs::path storedChat = SettingsToJson(settings).at("chat_model").get<std::string>();
    CHECK(storedChat.is_absolute());
}

TEST_CASE("settings.example.json loads with the current loader", "[settings]")
{
    const fs::path dir = morpheus::test::FreshTempDir("settings_example");
    fs::copy_file(morpheus::test::RepoDir() / "settings.example.json", dir / "settings.json");
    std::ifstream stream(dir / "settings.json");
    const nlohmann::json example = nlohmann::json::parse(stream);
    // The loader requires the model files to exist: create empty stand-ins at the example's relative paths.
    for (const char* key : {"chat_model", "embedding_model"}) {
        const fs::path model = dir / example.at(key).get<std::string>();
        fs::create_directories(model.parent_path());
        morpheus::test::WriteFile(model, "x");
    }
    const Settings settings = LoadSettings(dir / "settings.json");
    CHECK(settings.backend == BackendKind::LlamaCpp); // The example runs the models in-process.
    // The example turns the narration review and the compact transcript on.
    CHECK(settings.narrationCheck);
    CHECK(settings.compactTranscript);
    CHECK(settings.proseTurns == 2);
    CHECK(settings.budget.reviewMaxTokens == 384);
    CHECK(settings.budget.reviewThinkingTokens == 0);
    // A hosted run needs summary_cap below tail_allowance (the speaker's memories sit in the tail): the example's
    // budgets suit both kinds of run.
    CHECK(settings.budget.summaryCap == 1024);
    CHECK(settings.budget.tailAllowance == 3072);
    CHECK(settings.budget.summaryCap < settings.budget.tailAllowance);
}

TEST_CASE("LoadSettings reads the llama-server backend", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["backend"] = "llama-server";
    json["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 2}};
    json.erase("chat_model");
    json.erase("embedding_model");
    json.erase("gpu_layers");
    const Settings settings = LoadSettings(WriteSettings("settings_server", json));
    CHECK(settings.backend == BackendKind::LlamaServer);
    CHECK(settings.chatUrl == "http://127.0.0.1:8080");
    CHECK(settings.embeddingUrl == "http://127.0.0.1:8081");
    CHECK(settings.serverSlot == 2);
    CHECK(settings.chatModel.empty());
    CHECK(settings.contextSize == 16384); // Still what the budgets plan for.
    CHECK(settings.budget.narrateMaxTokens == 768);

    // The in-process backend's keys are ignored when present, even ones that would not load.
    json["chat_model"] = "models/missing.gguf";
    json["gpu_layers"] = "all";
    CHECK(LoadSettings(WriteSettings("settings_server_ignores_models", json)).chatModel.empty());

    // And llama-cpp ignores a server object.
    nlohmann::json local = ValidSettingsJson();
    local["server"] = {{"chat_url", "not a url"}};
    CHECK(LoadSettings(WriteSettings("settings_cpp_ignores_server", local)).backend == BackendKind::LlamaCpp);
}

TEST_CASE("LoadSettings names the backend key at fault", "[settings]")
{
    // Writes `json` and checks that loading it is a DataError naming `key`.
    auto refuses = [](const std::string& name, const nlohmann::json& json, const std::string& key) {
        INFO(name);
        const fs::path file = WriteSettings(name, json);
        CHECK_THROWS_AS(LoadSettings(file), DataError);
        CHECK_THROWS_WITH(LoadSettings(file), ContainsSubstring(key));
    };

    nlohmann::json missing = ValidSettingsJson();
    missing.erase("backend");
    refuses("settings_backend_missing", missing, "backend");
    nlohmann::json unknown = ValidSettingsJson();
    unknown["backend"] = "ollama";
    refuses("settings_backend_unknown", unknown, "backend");

    nlohmann::json server = ValidSettingsJson();
    server["backend"] = "llama-server";
    refuses("settings_server_missing", server, "server");
    server["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 0}};
    for (const std::string key : {"chat_url", "embedding_url", "slot"}) {
        nlohmann::json without = server;
        without["server"].erase(key);
        refuses("settings_server_no_" + key, without, key);
    }
    nlohmann::json https = server;
    https["server"]["chat_url"] = "https://127.0.0.1:8080"; // HTTPS is not supported.
    refuses("settings_server_https", https, "chat_url");
    nlohmann::json bare = server;
    bare["server"]["embedding_url"] = "127.0.0.1:8081";
    refuses("settings_server_bare", bare, "embedding_url");
    nlohmann::json empty = server;
    empty["server"]["chat_url"] = "http://";
    refuses("settings_server_empty_host", empty, "chat_url");
    nlohmann::json negative = server;
    negative["server"]["slot"] = -1;
    refuses("settings_server_slot", negative, "slot");
    nlohmann::json extra = server;
    extra["server"]["port"] = 8080;
    refuses("settings_server_extra", extra, "port");

    // llama-cpp still needs its model keys.
    for (const std::string key : {"chat_model", "embedding_model", "gpu_layers"}) {
        nlohmann::json without = ValidSettingsJson();
        without.erase(key);
        refuses("settings_cpp_no_" + key, without, key);
    }
}

TEST_CASE("SettingsToJson writes the backend and its own keys", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["backend"] = "llama-server";
    json["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 1}};
    const Settings original = LoadSettings(WriteSettings("settings_server_to_json_a", json));
    const nlohmann::json written = SettingsToJson(original);
    CHECK(written.at("backend") == "llama-server");
    CHECK(written.at("server") == json.at("server"));
    CHECK_FALSE(written.contains("chat_model")); // Only the backend in use has its keys written.
    CHECK_FALSE(written.contains("gpu_layers"));
    const Settings copy = LoadSettings(WriteSettings("settings_server_to_json_b", written));
    CHECK(copy.backend == BackendKind::LlamaServer);
    CHECK(copy.chatUrl == original.chatUrl);
    CHECK(copy.embeddingUrl == original.embeddingUrl);
    CHECK(copy.serverSlot == 1);

    // The in-process backend writes its model keys and no server object.
    const nlohmann::json local = SettingsToJson(LoadSettings(WriteSettings("settings_cpp_to_json", ValidSettingsJson())));
    CHECK(local.at("backend") == "llama-cpp");
    CHECK(local.contains("chat_model"));
    CHECK(local.contains("gpu_layers"));
    CHECK_FALSE(local.contains("server"));
}

TEST_CASE("LoadSettings reads an optional min_p per call type", "[settings]")
{
    // Absent: 0, so existing settings files load as they did.
    const Settings plain = LoadSettings(WriteSettings("settings_min_p_absent", ValidSettingsJson()));
    CHECK(plain.resolveSampling.minP == 0.0);
    CHECK(plain.narrateSampling.minP == 0.0);

    nlohmann::json json = ValidSettingsJson();
    json["sampling"]["narrate"]["min_p"] = 0.05;
    const Settings set = LoadSettings(WriteSettings("settings_min_p_set", json));
    CHECK(set.narrateSampling.minP == 0.05);
    CHECK(set.resolveSampling.minP == 0.0);
    // It round-trips.
    CHECK(LoadSettings(WriteSettings("settings_min_p_copy", SettingsToJson(set))).narrateSampling.minP == 0.05);

    for (const double bad : {-0.1, 1.0, 1.5}) {
        INFO(bad);
        nlohmann::json wrong = ValidSettingsJson();
        wrong["sampling"]["resolve"]["min_p"] = bad;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_min_p_bad", wrong)),
                          ContainsSubstring(std::format("sampling 'resolve': 'min_p' must be at least 0 and below 1, "
                                                        "not {}",
                                                        bad)));
    }
}
