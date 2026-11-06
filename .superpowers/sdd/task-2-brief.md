### Task 2: Settings for the turn layer

**Files:**
- Create: `src/core/MemoryMode.h`, `src/core/MemoryMode.cpp`
- Modify: `src/core/CMakeLists.txt` (add `MemoryMode.cpp`), `src/core/World.h`, `src/core/World.cpp`, `src/core/Settings.h`, `src/core/Settings.cpp`, `settings.example.json`, `tests/core/SettingsTests.cpp`
- Modify, but do not stage: the local `settings.json` at the repo root. Add `"retrieval_cap": 768` to its `budget`.

**Interfaces:**
- Consumes: `core::PathText` (Task 1).
- Produces:
  - `core/MemoryMode.h`: `enum class MemoryMode { Engine, Assisted }` (moved from `World.h`, same namespace), and `std::optional<MemoryMode> ParseMemoryMode(std::string_view)`
  - `BudgetSettings::retrievalCap` (the last member, so 7-value aggregate initializers in existing tests still compile)
  - `Settings::memoryMode` (`std::optional<MemoryMode>`)
  - `nlohmann::json SettingsToJson(const Settings&)`

- [ ] **Step 1: Write the failing tests**

In `tests/core/SettingsTests.cpp`:
- `ValidSettingsJson()` gains `"retrieval_cap": 768` in `budget`.
- Add `#include "core/MemoryMode.h"`.
- Append:

```cpp
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

TEST_CASE("LoadSettings keeps a margin for chat-template tokens", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    // The budget parts (retrieval is inside the tail) sum to 11008; 30 tokens more leaves no room for template
    // tokens and a transcript.
    json["context_size"] = 11008 + 30;
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_margin", json)), ContainsSubstring("no room for the transcript"));
}

TEST_CASE("SettingsToJson round-trips through LoadSettings", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["memory_mode"] = "assisted";
    const Settings original = LoadSettings(WriteSettings("settings_to_json_a", json));
    // Model paths in the JSON are absolute, so they still resolve from another folder.
    const Settings copy = LoadSettings(WriteSettings("settings_to_json_b", SettingsToJson(original)));
    CHECK(copy.chatModel == original.chatModel);
    CHECK(copy.embeddingDocumentPrefix == original.embeddingDocumentPrefix);
    CHECK(copy.contextSize == original.contextSize);
    CHECK(copy.budget.retrievalCap == original.budget.retrievalCap);
    CHECK(copy.budget.narrateAllowance == original.budget.narrateAllowance);
    CHECK(copy.narrateSampling.topP == original.narrateSampling.topP);
    CHECK(copy.memoryMode == MemoryMode::Assisted);
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
    CHECK_NOTHROW(LoadSettings(dir / "settings.json"));
}
```

Add `#include <fstream>` to the test file.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `core/MemoryMode.h` is missing, and `retrievalCap`, `memoryMode` and `SettingsToJson` are undeclared.

- [ ] **Step 3: Implement**

Create `src/core/MemoryMode.h`:

```cpp
#pragma once

#include <optional>
#include <string_view>

namespace morpheus::core {

// Who manages memory beyond the engine's deterministic retrieval (architecture spec, turn flow).
enum class MemoryMode { Engine, Assisted };

// "engine" or "assisted"; nullopt for anything else.
std::optional<MemoryMode> ParseMemoryMode(std::string_view text);

} // namespace morpheus::core
```

Create `src/core/MemoryMode.cpp`:

```cpp
#include "core/MemoryMode.h"

namespace morpheus::core {

std::optional<MemoryMode> ParseMemoryMode(std::string_view text)
{
    if (text == "engine") {
        return MemoryMode::Engine;
    }
    if (text == "assisted") {
        return MemoryMode::Assisted;
    }
    return std::nullopt;
}

} // namespace morpheus::core
```

`World.h`:
- Delete the `MemoryMode` enum and its comment.
- Add `#include "core/MemoryMode.h"`.

`World.cpp`: replace the `memory_mode` if/else chain with:

```cpp
        const std::string memoryMode = requireString("memory_mode");
        const std::optional<MemoryMode> parsedMode = ParseMemoryMode(memoryMode);
        if (!parsedMode) {
            throw DataError(std::format("{}: memory_mode must be 'engine' or 'assisted', got '{}'", worldWhere, memoryMode));
        }
        world.defaultMemoryMode = *parsedMode;
```

`Settings.h`:
- Include `<optional>`, `<nlohmann/json.hpp>` and `"core/MemoryMode.h"`.
- Append to `BudgetSettings`:

```cpp
    int retrievalCap = 0;     // T2: retrieved lore, history and notes. Part of the tail allowance.
```

- Append to `Settings`:

```cpp
    std::optional<MemoryMode> memoryMode = {}; // Overrides the world's default memory_mode when set.
```

- After `LoadSettings`:

```cpp
// The settings as settings.json JSON, with model paths absolute (UTF-8). Stored in a run's meta for reference.
nlohmann::json SettingsToJson(const Settings& settings);
```

Update the `LoadSettings` comment: "...and the budget must leave room for the transcript (L3) and SETTINGS_TEMPLATE_MARGIN chat-template tokens. memory_mode is optional."

`Settings.cpp`:
- At namespace scope, before `LoadSettings`:

```cpp
namespace {

// Chat-template tokens the budget sum must leave room for: the system message, the tail and the two messages the
// narrate call appends each carry a few. The exact figure is measured per prompt by context::AssembleResolvePrompt.
constexpr int SETTINGS_TEMPLATE_MARGIN = 64;

} // namespace
```

- The top-level `checkKeys` list gains `"memory_mode"`, and the budget list gains `"retrieval_cap"`.
- After the `gpu_layers` line:

```cpp
    if (json.contains("memory_mode")) {
        const std::string text = requireString(json, "memory_mode", "settings");
        settings.memoryMode = ParseMemoryMode(text);
        if (!settings.memoryMode) {
            throw DataError(std::format("{}: 'memory_mode' must be 'engine' or 'assisted', got '{}'", where, text));
        }
    }
```

- After `summaryCap` is read:

```cpp
    settings.budget.retrievalCap = requireInt(budget, "retrieval_cap", "budget", 1, settings.budget.tailAllowance);
```

- The room check becomes `if (fixed + SETTINGS_TEMPLATE_MARGIN >= contextSize)`. The sum stays without `retrievalCap`, which lives inside the tail.
- Add `SettingsToJson`:

```cpp
nlohmann::json SettingsToJson(const Settings& settings)
{
    auto sampling = [](const SamplingSettings& values) {
        return nlohmann::json{{"temperature", values.temperature}, {"top_k", values.topK}, {"top_p", values.topP}};
    };
    nlohmann::json json = {
        {"chat_model", PathText(settings.chatModel)},
        {"embedding_model", PathText(settings.embeddingModel)},
        {"embedding_query_prefix", settings.embeddingQueryPrefix},
        {"embedding_document_prefix", settings.embeddingDocumentPrefix},
        {"context_size", settings.contextSize},
        {"gpu_layers", settings.gpuLayers},
        {"budget",
         {{"resolve_max_tokens", settings.budget.resolveMaxTokens},
          {"narrate_max_tokens", settings.budget.narrateMaxTokens},
          {"narrate_allowance", settings.budget.narrateAllowance},
          {"tail_allowance", settings.budget.tailAllowance},
          {"engine_rules_cap", settings.budget.engineRulesCap},
          {"world_cap", settings.budget.worldCap},
          {"summary_cap", settings.budget.summaryCap},
          {"retrieval_cap", settings.budget.retrievalCap}}},
        {"sampling", {{"resolve", sampling(settings.resolveSampling)}, {"narrate", sampling(settings.narrateSampling)}}},
    };
    if (settings.memoryMode) {
        json["memory_mode"] = *settings.memoryMode == MemoryMode::Engine ? "engine" : "assisted";
    }
    return json;
}
```

Add `MemoryMode.cpp` to `src/core/CMakeLists.txt`. In `settings.example.json`, add `"retrieval_cap": 768` after `"summary_cap": 2048` in `budget`. Make the same edit to the local `settings.json`, without staging it.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings],[world]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/core tests/core settings.example.json
git commit -m "Add retrieval_cap, memory_mode and a template margin to settings" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

