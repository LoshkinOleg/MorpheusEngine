### Task 7: The backend keys in settings.json

`settings.json` names its backend. Under `llama-cpp` the model keys stay required; under `llama-server` a `server` object is, and the model keys are ignored. `context_size`, the prefixes, the budget and the sampling stay required for both.

**Files:**
- Modify: `src/core/Settings.h`, `src/core/Settings.cpp`
- Modify: `settings.example.json`
- Test: `tests/core/SettingsTests.cpp`
- This machine only, never staged: `settings.json`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: in `core/Settings.h`: `enum class BackendKind { LlamaCpp, LlamaServer };`, `std::optional<BackendKind> ParseBackendKind(std::string_view text)` (`"llama-cpp"`, `"llama-server"`, else nullopt), `std::string BackendKindName(BackendKind kind)`; `core::Settings` fields `BackendKind backend = BackendKind::LlamaCpp`, `std::string chatUrl`, `std::string embeddingUrl`, `int serverSlot = 0`. `LoadSettings` validation messages name the key (`settings needs 'backend'`, `'backend' must be 'llama-cpp' or 'llama-server', got '...'`, `settings needs 'server'`, `server needs 'chat_url'`, `server 'chat_url' must start with 'http://' ...`, `server 'slot' must be an integer in 0..2147483647`, `unknown key '...' in server`). `SettingsToJson` writes `backend`, plus `chat_model`, `embedding_model`, `gpu_layers` under `llama-cpp` or `server` under `llama-server`.

- [ ] **Step 1: Write the failing tests**

In `tests/core/SettingsTests.cpp`, in `ValidSettingsJson()`, make `"backend": "llama-cpp",` the document's first key (before `"chat_model"`).

In the test case "SettingsToJson round-trips through LoadSettings", add after `CHECK(copy.chatModel == original.chatModel);`:

```cpp
    CHECK(copy.backend == BackendKind::LlamaCpp);
```

In the test case "settings.example.json loads with the current loader", add after `const Settings settings = LoadSettings(dir / "settings.json");`:

```cpp
    CHECK(settings.backend == BackendKind::LlamaCpp); // The example runs the models in-process.
```

Append:

```cpp
TEST_CASE("LoadSettings reads the llama-server backend", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["backend"] = "llama-server";
    json["server"] = {{"chat_url", "http://127.0.0.1:8080"}, {"embedding_url", "http://localhost:8081"}, {"slot", 2}};
    json.erase("chat_model");
    json.erase("embedding_model");
    json.erase("gpu_layers");
    const Settings settings = LoadSettings(WriteSettings("settings_server", json));
    CHECK(settings.backend == BackendKind::LlamaServer);
    CHECK(settings.chatUrl == "http://127.0.0.1:8080");
    CHECK(settings.embeddingUrl == "http://localhost:8081");
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
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/core/SettingsTests.cpp`: `'BackendKind': undeclared identifier`, `'backend': is not a member of 'morpheus::core::Settings'`.

- [ ] **Step 3: Add the backend to Settings**

In `src/core/Settings.h`, add `#include <string_view>` to the standard includes, and after the `MIN_THINKING_TOKENS` constant insert:

```cpp

// Which backend the app and the tools build: llama.cpp in-process, or llama-server over HTTP.
enum class BackendKind { LlamaCpp, LlamaServer };

// "llama-cpp" or "llama-server"; nullopt for anything else.
std::optional<BackendKind> ParseBackendKind(std::string_view text);

// The settings.json spelling: "llama-cpp" or "llama-server".
std::string BackendKindName(BackendKind kind);
```

In `struct Settings`, replace

```cpp
struct Settings {
    std::filesystem::path chatModel = {};      // Relative paths are resolved against the settings file's folder.
    std::filesystem::path embeddingModel = {};
```

with

```cpp
struct Settings {
    BackendKind backend = BackendKind::LlamaCpp;
    // llama-cpp only (empty under llama-server): the model files. Relative paths are resolved against the settings
    // file's folder.
    std::filesystem::path chatModel = {};
    std::filesystem::path embeddingModel = {};
```

replace

```cpp
    int gpuLayers = 0;                         // -1 puts every layer on the GPU.
```

with

```cpp
    int gpuLayers = 0;                         // llama-cpp only. -1 puts every layer on the GPU.
    // llama-server only (empty under llama-cpp): "http://host:port" of the chat and embedding servers, and the server
    // slot Morpheus's own calls use.
    std::string chatUrl = {};
    std::string embeddingUrl = {};
    int serverSlot = 0;
```

and replace the comment of `LoadSettings`

```cpp
// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors,
// both model files must exist, and the budget must leave room for the transcript (L3) and SETTINGS_TEMPLATE_MARGIN
// chat-template tokens; the review budgets are not part of that sum. memory_mode is optional; every other key is
// required.
```

with

```cpp
// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors.
// `backend` says which keys the models need: under llama-cpp chat_model, embedding_model (both files must exist) and
// gpu_layers; under llama-server a `server` object with chat_url and embedding_url (each "http://" and a host) and
// slot (at least 0). Each backend's keys are ignored, not checked, under the other. The budget must leave room for the
// transcript (L3) and SETTINGS_TEMPLATE_MARGIN chat-template tokens; the review budgets are not part of that sum.
// memory_mode is optional; every other key is required.
```

and the comment of `SettingsToJson`

```cpp
// The settings as settings.json JSON, with model paths absolute (UTF-8). Stored in a run's meta for reference.
```

with

```cpp
// The settings as settings.json JSON, with model paths absolute (UTF-8), and only the keys of the backend in use.
// Stored in a run's meta for reference.
```

In `src/core/Settings.cpp`, add `#include <string_view>` to the standard includes. After the anonymous namespace (before `Settings LoadSettings(`), add:

```cpp
std::optional<BackendKind> ParseBackendKind(std::string_view text)
{
    if (text == "llama-cpp") {
        return BackendKind::LlamaCpp;
    }
    if (text == "llama-server") {
        return BackendKind::LlamaServer;
    }
    return std::nullopt;
}

std::string BackendKindName(BackendKind kind)
{
    switch (kind) {
    case BackendKind::LlamaCpp:
        return "llama-cpp";
    case BackendKind::LlamaServer:
        return "llama-server";
    }
    throw ContractError("BackendKindName: unhandled BackendKind");
}

```

In `LoadSettings`, replace

```cpp
    checkKeys(json, "settings", {"chat_model", "embedding_model", "embedding_query_prefix", "embedding_document_prefix",
                                 "context_size", "gpu_layers", "budget", "sampling", "memory_mode", "narration_check",
                                 "compact_transcript", "prose_turns"});
```

with

```cpp
    checkKeys(json, "settings", {"backend", "chat_model", "embedding_model", "embedding_query_prefix",
                                 "embedding_document_prefix", "context_size", "gpu_layers", "server", "budget",
                                 "sampling", "memory_mode", "narration_check", "compact_transcript", "prose_turns"});
```

and replace

```cpp
    settings.chatModel = requireModel("chat_model");
    settings.embeddingModel = requireModel("embedding_model");
    settings.embeddingQueryPrefix = requireString(json, "embedding_query_prefix", "settings");
    settings.embeddingDocumentPrefix = requireString(json, "embedding_document_prefix", "settings");
    settings.contextSize = requireInt(json, "context_size", "settings", 512, 262144);
    settings.gpuLayers = requireInt(json, "gpu_layers", "settings", -1, 10000);
```

with

```cpp
    // The backend says which keys reach the models. Each backend's own keys are ignored (not read or checked) under
    // the other, so a file can keep both while it switches.
    const std::string backendText = requireString(json, "backend", "settings");
    const std::optional<BackendKind> backend = ParseBackendKind(backendText);
    if (!backend) {
        throw DataError(std::format("{}: 'backend' must be 'llama-cpp' or 'llama-server', got '{}'", where, backendText));
    }
    settings.backend = *backend;
    if (settings.backend == BackendKind::LlamaCpp) {
        settings.chatModel = requireModel("chat_model");
        settings.embeddingModel = requireModel("embedding_model");
        settings.gpuLayers = requireInt(json, "gpu_layers", "settings", -1, 10000);
    } else {
        // The servers: plain HTTP to a host (localhost is the intended use), and the slot Morpheus's calls use.
        const nlohmann::json& server = requireMember(json, "server", "settings");
        checkKeys(server, "server", {"chat_url", "embedding_url", "slot"});
        auto requireUrl = [&](const char* key) {
            const std::string url = requireString(server, key, "server");
            if (!url.starts_with("http://") || url.size() == std::string_view("http://").size()) {
                throw DataError(std::format("{}: server '{}' must start with 'http://' and name a host (HTTPS is not "
                                            "supported), got '{}'",
                                            where, key, url));
            }
            return url;
        };
        settings.chatUrl = requireUrl("chat_url");
        settings.embeddingUrl = requireUrl("embedding_url");
        settings.serverSlot = requireInt(server, "slot", "server", 0, std::numeric_limits<int>::max());
    }
    settings.embeddingQueryPrefix = requireString(json, "embedding_query_prefix", "settings");
    settings.embeddingDocumentPrefix = requireString(json, "embedding_document_prefix", "settings");
    settings.contextSize = requireInt(json, "context_size", "settings", 512, 262144);
```

In `SettingsToJson`, replace

```cpp
    nlohmann::json json = {
        {"chat_model", PathText(settings.chatModel)},
        {"embedding_model", PathText(settings.embeddingModel)},
        {"embedding_query_prefix", settings.embeddingQueryPrefix},
        {"embedding_document_prefix", settings.embeddingDocumentPrefix},
        {"context_size", settings.contextSize},
        {"gpu_layers", settings.gpuLayers},
```

with

```cpp
    nlohmann::json json = {
        {"backend", BackendKindName(settings.backend)},
        {"embedding_query_prefix", settings.embeddingQueryPrefix},
        {"embedding_document_prefix", settings.embeddingDocumentPrefix},
        {"context_size", settings.contextSize},
```

and replace

```cpp
    if (settings.memoryMode) {
        json["memory_mode"] = *settings.memoryMode == MemoryMode::Engine ? "engine" : "assisted";
    }
    return json;
```

with

```cpp
    if (settings.memoryMode) {
        json["memory_mode"] = *settings.memoryMode == MemoryMode::Engine ? "engine" : "assisted";
    }
    // Only the backend in use has its keys.
    if (settings.backend == BackendKind::LlamaCpp) {
        json["chat_model"] = PathText(settings.chatModel);
        json["embedding_model"] = PathText(settings.embeddingModel);
        json["gpu_layers"] = settings.gpuLayers;
    } else {
        json["server"] = {{"chat_url", settings.chatUrl}, {"embedding_url", settings.embeddingUrl}, {"slot", settings.serverSlot}};
    }
    return json;
```

In `settings.example.json`, add `"backend": "llama-cpp",` as the first key:

```json
{
  "backend": "llama-cpp",
  "chat_model": "models/Qwen3.8-27B-UD-Q4_K_M.gguf",
```

(the rest of the file unchanged).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings]"`
Expected: `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 403` (the app tests copy `settings.example.json`, which now names its backend).

- [ ] **Step 5: Update this machine's settings.json (never staged)**

This machine's `settings.json` (gitignored) has no `backend` key, so the app and the tools would refuse it now. Add `"backend": "llama-cpp",` as its first key, by hand or with:

```bash
perl -0pi -e 's/\A\{(\r?\n)/{$1  "backend": "llama-cpp",$1/' settings.json && head -3 settings.json
```

Expected: the second line is `  "backend": "llama-cpp",`. Then check it loads: `cmake --build build/dev --config RelWithDebInfo --target morpheus_probe` and `./build/dev/bin/RelWithDebInfo/morpheus_probe.exe settings.json game_projects/sandcrawler "I look around."` streams two completions (it needs the GPU; skip the run if the models are not in `models/`). `git status --short` must not list `settings.json`.

- [ ] **Step 6: Byte check and commit**

```bash
check_bytes src/core/Settings.h src/core/Settings.cpp settings.example.json tests/core/SettingsTests.cpp
git add src/core/Settings.h src/core/Settings.cpp settings.example.json tests/core/SettingsTests.cpp
git commit -m "settings.json chooses the backend: llama-cpp or llama-server with its server URLs and slot" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

