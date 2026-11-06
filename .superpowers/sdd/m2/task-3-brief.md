### Task 3: Settings

**Files:**
- Create: `src/core/Settings.h`, `src/core/Settings.cpp`, `tests/core/SettingsTests.cpp`, `settings.example.json`
- Modify: `src/core/CMakeLists.txt` (add `Settings.cpp`), `.gitignore`

**Interfaces:**
- Consumes: `JsonIntInRange`, `DataError`.
- Produces (namespace `morpheus::core`):
  - `struct SamplingSettings { double temperature; int topK; double topP; }`
  - `struct BudgetSettings { int resolveMaxTokens, narrateMaxTokens, tailAllowance, engineRulesCap, worldCap, summaryCap; }`
  - `struct Settings { std::filesystem::path chatModel, embeddingModel; std::string embeddingQueryPrefix, embeddingDocumentPrefix; int contextSize; int gpuLayers; BudgetSettings budget; SamplingSettings resolveSampling, narrateSampling; }`
  - `Settings LoadSettings(const std::filesystem::path& file)`

- [ ] **Step 1: Write the failing test `tests/core/SettingsTests.cpp`**

```cpp
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
            "resolve_max_tokens": 512, "narrate_max_tokens": 768, "tail_allowance": 2048,
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
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Settings.h'`.

- [ ] **Step 3: Create `src/core/Settings.h`**

```cpp
#pragma once

#include <filesystem>
#include <string>

namespace morpheus::core {

struct SamplingSettings {
    double temperature = 0.0; // 0 means greedy decoding.
    int topK = 40;
    double topP = 0.95;
};

// Token budget for one assembled prompt (architecture spec, "Context layout and KV cache").
struct BudgetSettings {
    int resolveMaxTokens = 0; // Generation reserved for the resolve call.
    int narrateMaxTokens = 0; // Generation reserved for the narrate call.
    int tailAllowance = 0;    // State view + retrieved snippets + player input + instruction (T1-T4).
    int engineRulesCap = 0;   // L0
    int worldCap = 0;         // L1: world rules + always-on lore.
    int summaryCap = 0;       // L2
};

// settings.json: per-machine configuration. Not committed; settings.example.json documents it.
struct Settings {
    std::filesystem::path chatModel = {};      // Relative paths are resolved against the settings file's folder.
    std::filesystem::path embeddingModel = {};
    std::string embeddingQueryPrefix = {};     // Prepended to retrieval queries (model-specific).
    std::string embeddingDocumentPrefix = {};  // Prepended to embedded documents (model-specific).
    int contextSize = 0;
    int gpuLayers = 0;                         // -1 puts every layer on the GPU.
    BudgetSettings budget = {};
    SamplingSettings resolveSampling = {};
    SamplingSettings narrateSampling = {};
};

// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors,
// both model files must exist, and the budget must leave room for the transcript (L3).
Settings LoadSettings(const std::filesystem::path& file);

} // namespace morpheus::core
```

- [ ] **Step 4: Create `src/core/Settings.cpp`, and add it to `src/core/CMakeLists.txt` after `JsonInt.cpp`**

```cpp
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
```

- [ ] **Step 5: Create `settings.example.json` and update `.gitignore`**

`settings.example.json`, at the repo root:

```json
{
  "chat_model": "models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
  "embedding_model": "models/nomic-embed-text-v1.5.Q8_0.gguf",
  "embedding_query_prefix": "search_query: ",
  "embedding_document_prefix": "search_document: ",
  "context_size": 16384,
  "gpu_layers": -1,
  "budget": {
    "resolve_max_tokens": 512,
    "narrate_max_tokens": 768,
    "tail_allowance": 2048,
    "engine_rules_cap": 1024,
    "world_cap": 4096,
    "summary_cap": 2048
  },
  "sampling": {
    "resolve": { "temperature": 0.3, "top_k": 40, "top_p": 0.9 },
    "narrate": { "temperature": 0.8, "top_k": 40, "top_p": 0.95 }
  }
}
```

Append to `.gitignore`:

```
models/
settings.json
```

- [ ] **Step 6: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `[settings]`.

- [ ] **Step 7: Commit**

```bash
git add src/core tests/core/SettingsTests.cpp settings.example.json .gitignore
git commit -m "Add settings.json loading" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

