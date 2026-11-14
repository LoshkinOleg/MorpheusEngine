### Task 8: The backend in the Settings window

`SettingsEdit` reads and writes `backend` and the server keys; the Settings window edits the backend (a choice) and, under `llama-server`, the two URLs and the slot; the model-path fields and GPU layers show only under `llama-cpp`.

**Files:**
- Modify: `src/app/model/SettingsEdit.h`, `src/app/model/SettingsEdit.cpp`
- Modify: `src/app/ui/SettingsWindow.cpp`
- Test: `tests/app/SettingsEditTests.cpp`, `tests/app/UiTests.cpp`

**Interfaces:**
- Consumes: `core::BackendKind`, `core::ParseBackendKind`, `core::BackendKindName`, `core::Settings::backend`, `chatUrl`, `embeddingUrl`, `serverSlot` (Task 7).
- Produces: `app::SettingsEdit` fields `core::BackendKind backend = core::BackendKind::LlamaCpp`, `std::string chatUrl`, `std::string embeddingUrl`, `int serverSlot = 0`. `ReadSettingsEdit` requires the active backend's keys and reads the other backend's keys when present (empty or 0 otherwise); `SaveSettingsEdit` writes `backend` and the active backend's keys, and keeps the other backend's keys as the file has them.

- [ ] **Step 1: Write the failing tests**

In `tests/app/SettingsEditTests.cpp`, in the test case "ReadSettingsEdit reads the edited values as the file writes them", add after `const app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");`:

```cpp
    CHECK(edit.backend == core::BackendKind::LlamaCpp); // As settings.example.json sets it.
    CHECK(edit.chatUrl.empty());                        // The example has no server object.
```

Append:

```cpp
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
```

In `tests/app/UiTests.cpp`, add `#include "core/Settings.h"` after `#include "core/MemoryMode.h"`, and append this draw smoke test (the window's state is private; the test proves the llama-server branch draws without an ImGui assertion):

```cpp
TEST_CASE("The Settings window draws the llama-server backend's fields", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_settings_server");
    // settings.json on llama-server, without the in-process backend's keys: the URLs and the slot replace them.
    nlohmann::json settings = nlohmann::json::parse(test::ReadFile(dir / "settings.json"));
    settings.erase("chat_model");
    settings.erase("embedding_model");
    settings.erase("gpu_layers");
    settings["backend"] = "llama-server";
    settings["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://127.0.0.1:8081"}, {"slot", 0}};
    test::WriteFile(dir / "settings.json", settings.dump(2));
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::SettingsWindow window;

    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    for (int frame = 0; frame < 2; ++frame) {
        controller.Frame();
        imgui.Frame([&] { window.Draw(controller); });
    }
    CHECK(controller.Engine().CurrentSettings().backend == core::BackendKind::LlamaServer);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/app/SettingsEditTests.cpp`: `'backend': is not a member of 'morpheus::app::SettingsEdit'` (and `chatUrl`, `embeddingUrl`, `serverSlot`).

- [ ] **Step 3: Read and write the backend keys**

In `src/app/model/SettingsEdit.h`, replace

```cpp
struct SettingsEdit {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int contextSize = 0;
    int gpuLayers = 0; // -1 puts every layer on the GPU.
```

with

```cpp
struct SettingsEdit {
    core::BackendKind backend = core::BackendKind::LlamaCpp;
    // llama-cpp: the models and their GPU layers. Kept as the file has them under llama-server ("" and 0 when absent).
    std::string chatModel = {};
    std::string embeddingModel = {};
    int gpuLayers = 0; // -1 puts every layer on the GPU.
    // llama-server: server.chat_url, server.embedding_url and server.slot. Kept as the file has them under llama-cpp
    // ("" and 0 when absent).
    std::string chatUrl = {};
    std::string embeddingUrl = {};
    int serverSlot = 0;
    int contextSize = 0;
```

and replace the comment of `ReadSettingsEdit`

```cpp
// Reads the edited values from a settings file. Throws DataError naming the file when it cannot be read, is not
// JSON, or lacks one of them (core::LoadSettings explains a bad file in full).
```

with

```cpp
// Reads the edited values from a settings file: the backend in use needs its keys, the other backend's keys are read
// when present. Throws DataError naming the file when it cannot be read, is not JSON, or lacks one of the values it
// needs (core::LoadSettings explains a bad file in full).
```

and the first sentence of the comment of `SaveSettingsEdit`

```cpp
// Writes `edit` into the settings file, keeping its other keys (the embedding prefixes, the rest of the budget) and
// their order.
```

with

```cpp
// Writes `edit` into the settings file: `backend` and the keys of the backend it names (the other backend's keys stay
// as the file has them), keeping the file's other keys (the embedding prefixes, the rest of the budget) and their
// order.
```

In `src/app/model/SettingsEdit.cpp`, in `ReadSettingsEdit`, replace

```cpp
        SettingsEdit edit;
        edit.chatModel = json.at("chat_model").get<std::string>();
        edit.embeddingModel = json.at("embedding_model").get<std::string>();
        edit.contextSize = json.at("context_size").get<int>();
        edit.gpuLayers = json.at("gpu_layers").get<int>();
```

with

```cpp
        SettingsEdit edit;
        const std::string backendText = json.at("backend").get<std::string>();
        const std::optional<core::BackendKind> backend = core::ParseBackendKind(backendText);
        if (!backend) {
            throw core::DataError(
                std::format("{}: 'backend' must be 'llama-cpp' or 'llama-server'", core::PathText(file)));
        }
        edit.backend = *backend;
        // The backend in use needs its keys; the other backend's are read when the file has them, so a switch in the
        // window starts from them.
        const bool local = edit.backend == core::BackendKind::LlamaCpp;
        if (local || json.contains("chat_model")) {
            edit.chatModel = json.at("chat_model").get<std::string>();
        }
        if (local || json.contains("embedding_model")) {
            edit.embeddingModel = json.at("embedding_model").get<std::string>();
        }
        if (local || json.contains("gpu_layers")) {
            edit.gpuLayers = json.at("gpu_layers").get<int>();
        }
        if (!local || json.contains("server")) {
            const nlohmann::ordered_json& server = json.at("server");
            if (!local || server.contains("chat_url")) {
                edit.chatUrl = server.at("chat_url").get<std::string>();
            }
            if (!local || server.contains("embedding_url")) {
                edit.embeddingUrl = server.at("embedding_url").get<std::string>();
            }
            if (!local || server.contains("slot")) {
                edit.serverSlot = server.at("slot").get<int>();
            }
        }
        edit.contextSize = json.at("context_size").get<int>();
```

Add `#include <optional>` to the file's standard includes.

In `SaveSettingsEdit`, replace

```cpp
        json["chat_model"] = edit.chatModel;
        json["embedding_model"] = edit.embeddingModel;
        json["context_size"] = edit.contextSize;
        json["gpu_layers"] = edit.gpuLayers;
```

with

```cpp
        // The backend and its own keys; the other backend's keys stay as the file has them.
        json["backend"] = core::BackendKindName(edit.backend);
        if (edit.backend == core::BackendKind::LlamaCpp) {
            json["chat_model"] = edit.chatModel;
            json["embedding_model"] = edit.embeddingModel;
            json["gpu_layers"] = edit.gpuLayers;
        } else {
            json["server"]["chat_url"] = edit.chatUrl;
            json["server"]["embedding_url"] = edit.embeddingUrl;
            json["server"]["slot"] = edit.serverSlot;
        }
        json["context_size"] = edit.contextSize;
```

- [ ] **Step 4: Edit the backend in the Settings window**

In `src/app/ui/SettingsWindow.cpp`, add `#include <imgui_stdlib.h>` after `#include <imgui.h>`. In the anonymous namespace, after `MEMORY_MODES`, add:

```cpp
// Combo items, in core::BackendKind's order.
constexpr const char* BACKENDS[] = {"llama.cpp, in-process (llama-cpp)", "llama-server over HTTP (llama-server)"};
```

Replace

```cpp
            ImGui::SeparatorText("Models");
            modelCombo("Chat model", edit.chatModel);
            modelCombo("Embedding model", edit.embeddingModel);
            ImGui::InputInt("Context size", &edit.contextSize, 1024, 4096);
            ImGui::InputInt("GPU layers", &edit.gpuLayers);
            ImGui::SetItemTooltip("-1 puts every layer on the GPU.");
```

with

```cpp
            // The backend: models in-process, picked from the models folder, or two llama-server URLs and a slot.
            // Only the fields of the backend chosen show; the other's stay in settings.json.
            ImGui::SeparatorText("Models");
            int backend = edit.backend == core::BackendKind::LlamaCpp ? 0 : 1;
            if (ImGui::Combo("Backend", &backend, BACKENDS, IM_ARRAYSIZE(BACKENDS))) {
                edit.backend = backend == 0 ? core::BackendKind::LlamaCpp : core::BackendKind::LlamaServer;
            }
            if (edit.backend == core::BackendKind::LlamaCpp) {
                modelCombo("Chat model", edit.chatModel);
                modelCombo("Embedding model", edit.embeddingModel);
            } else {
                ImGui::InputText("Chat server URL", &edit.chatUrl);
                ImGui::SetItemTooltip("http://host:port of the chat model's llama-server, e.g. http://127.0.0.1:8080.");
                ImGui::InputText("Embedding server URL", &edit.embeddingUrl);
                ImGui::SetItemTooltip("http://host:port of the embedding model's llama-server (started with "
                                      "--embedding), e.g. http://127.0.0.1:8081.");
                ImGui::InputInt("Server slot", &edit.serverSlot);
                ImGui::SetItemTooltip("The chat server's slot for Morpheus's calls (0 or more). Each slot keeps its "
                                      "own prompt cache.");
            }
            ImGui::InputInt("Context size", &edit.contextSize, 1024, 4096);
            if (edit.backend == core::BackendKind::LlamaServer) {
                ImGui::SetItemTooltip("What the budgets plan for. Each of the chat server's slots must hold at least "
                                      "this many tokens.");
            }
            if (edit.backend == core::BackendKind::LlamaCpp) {
                ImGui::InputInt("GPU layers", &edit.gpuLayers);
                ImGui::SetItemTooltip("-1 puts every layer on the GPU.");
            }
```

and replace

```cpp
            ImGui::TextDisabled("Apply saves settings.json and restarts the engine. New model paths, context size or "
                                "GPU layers reload the models.");
```

with

```cpp
            ImGui::TextDisabled("Apply saves settings.json and restarts the engine. A new backend, model paths, server "
                                "URLs or slot, context size or GPU layers reload the backend.");
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit],[ui]"`
Expected: `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 407`.

- [ ] **Step 6: Byte check and commit**

```bash
check_bytes src/app/model/SettingsEdit.h src/app/model/SettingsEdit.cpp src/app/ui/SettingsWindow.cpp tests/app/SettingsEditTests.cpp tests/app/UiTests.cpp
git add src/app/model/SettingsEdit.h src/app/model/SettingsEdit.cpp src/app/ui/SettingsWindow.cpp tests/app/SettingsEditTests.cpp tests/app/UiTests.cpp
git commit -m "Edit the backend, the server URLs and the slot in the Settings window" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

