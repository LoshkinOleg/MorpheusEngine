### Task 4: The Settings window's values, read and saved

The Settings window edits the model paths, context size, GPU layers, sampling and memory mode. Saving patches the file's own JSON, so the keys the window does not touch and the relative model paths stay as written. The result is checked with `core::LoadSettings` before it replaces the file.

**Files:**
- Create: `src/app/model/SettingsEdit.h`, `src/app/model/SettingsEdit.cpp`, `tests/app/AppTestSupport.h`, `tests/app/SettingsEditTests.cpp`
- Modify: `src/app/CMakeLists.txt` (add `model/SettingsEdit.cpp`)

**Interfaces:**
- Consumes: `core::LoadSettings`, `core::Settings`, `core::SamplingSettings`, `core::MemoryMode`, `core::ParseMemoryMode`, `core::PathText`.
- Produces:
  - `struct app::SettingsEdit { std::string chatModel; std::string embeddingModel; int contextSize; int gpuLayers; core::SamplingSettings resolveSampling; core::SamplingSettings narrateSampling; std::optional<core::MemoryMode> memoryMode; }`.
  - `app::SettingsEdit app::ReadSettingsEdit(const std::filesystem::path& file)`.
  - `core::Settings app::SaveSettingsEdit(const std::filesystem::path& file, const SettingsEdit& edit)`.
  - Test support: `test::SettingsFolder(std::string_view name)` and `test::ReadFile(const std::filesystem::path&)`.

- [ ] **Step 1: Write the test support and the failing tests**

Create `tests/app/AppTestSupport.h`:

```cpp
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
```

Create `tests/app/SettingsEditTests.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Add `model/SettingsEdit.cpp` to `add_library(morpheus_app_model ...)` in `src/app/CMakeLists.txt`.

Run: `cmake --build --preset tests`
Expected: a compile error: `app/model/SettingsEdit.h` is missing.

- [ ] **Step 3: Write the header**

Create `src/app/model/SettingsEdit.h`:

```cpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/MemoryMode.h"
#include "core/Settings.h"

namespace morpheus::app {

// The settings.json values the Settings window edits. Model paths stay as written in the file (often relative to
// its folder), so saving does not rewrite them as absolute paths.
struct SettingsEdit {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int contextSize = 0;
    int gpuLayers = 0; // -1 puts every layer on the GPU.
    core::SamplingSettings resolveSampling = {};
    core::SamplingSettings narrateSampling = {};
    std::optional<core::MemoryMode> memoryMode = {}; // nullopt: each world's own default.
};

// Reads the edited values from a settings file. Throws DataError naming the file when it cannot be read, is not
// JSON, or lacks one of them (core::LoadSettings explains a bad file in full).
SettingsEdit ReadSettingsEdit(const std::filesystem::path& file);

// Writes `edit` into the settings file, keeping its other keys (the embedding prefixes, the budget) and their order.
// The new text is first written beside the file as "<name>.new" and loaded with core::LoadSettings. When that throws
// DataError, the check file is removed, the settings file is left as it was, and the error propagates. Otherwise the
// check file replaces the settings file. Returns the loaded settings.
core::Settings SaveSettingsEdit(const std::filesystem::path& file, const SettingsEdit& edit);

} // namespace morpheus::app
```

- [ ] **Step 4: Write the implementation**

Create `src/app/model/SettingsEdit.cpp`:

```cpp
#include "app/model/SettingsEdit.h"

#include <format>
#include <fstream>
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

    // Check the result with the real loader before replacing anything. The check file sits beside the settings
    // file, so relative model paths resolve the same way.
    std::filesystem::path checkFile = file;
    checkFile += ".new";
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
    } catch (const core::DataError&) {
        // The settings error is what the player needs; a check file that will not delete must not replace it.
        std::error_code ignored;
        std::filesystem::remove(checkFile, ignored);
        throw;
    }
    std::filesystem::rename(checkFile, file); // Replaces the settings file in one step.
    return settings;
}

} // namespace morpheus::app
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit]"`
Expected: all four tests pass. The invalid-edit message names `settings.json.new`, the file that was checked; that is expected. Then `ctest --preset tests`: everything passes.

- [ ] **Step 6: Commit**

```bash
git add src/app/CMakeLists.txt src/app/model/SettingsEdit.h src/app/model/SettingsEdit.cpp tests/app/AppTestSupport.h tests/app/SettingsEditTests.cpp
git commit -m "Read and save the app's editable settings" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

