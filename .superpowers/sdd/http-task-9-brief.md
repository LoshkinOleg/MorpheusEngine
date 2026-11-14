### Task 9: The callers build the backend settings.json names

The engine host keeps a loaded backend only while the backend kind, the URLs and the slot are unchanged too; the app chooses its factory by `settings.backend`; `morpheus_play` and `morpheus_probe` build either backend.

**Files:**
- Modify: `src/app/model/EngineHost.h`, `src/app/model/EngineHost.cpp`
- Modify: `src/app/main.cpp`, `src/app/CMakeLists.txt`
- Modify: `tools/play/main.cpp`, `tools/play/CMakeLists.txt`
- Modify: `tools/probe/main.cpp`, `tools/probe/CMakeLists.txt`
- Test: `tests/app/EngineHostTests.cpp`, `tests/app/AppControllerTests.cpp`

**Interfaces:**
- Consumes: `core::Settings::backend`, `chatUrl`, `embeddingUrl`, `serverSlot` (Task 7); `llamaserver::LlamaServerBackend`, `LlamaServerConfig` (Tasks 5-6); `app::SettingsEdit`'s backend fields (Task 8).
- Produces: `app::EngineHost::BackendFactory app::BackendFactoryByKind(EngineHost::BackendFactory llamaCpp, EngineHost::BackendFactory llamaServer)` in `app/model/EngineHost.h`.

- [ ] **Step 1: Write the failing tests**

In `tests/app/EngineHostTests.cpp`, append:

```cpp
TEST_CASE("EngineHost rebuilds the backend when the backend kind, a server URL or the slot changes", "[engine_host]")
{
    std::atomic<int> made = 0;
    app::EngineHost host(CountingFactory(made));
    core::Settings server = test::EngineSettings();
    server.backend = core::BackendKind::LlamaServer;
    server.chatUrl = "http://127.0.0.1:8080";
    server.embeddingUrl = "http://127.0.0.1:8081";
    host.Start(server);
    PollWhileLoading(host);
    CHECK(made == 1);

    // The same servers: the backend is kept.
    host.Start(server);
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Ready);
    CHECK(made == 1);

    // Each change loads a new one.
    core::Settings moved = server;
    moved.chatUrl = "http://127.0.0.1:9090";
    host.Start(moved);
    PollWhileLoading(host);
    CHECK(made == 2);
    core::Settings otherEmbedding = moved;
    otherEmbedding.embeddingUrl = "http://127.0.0.1:9091";
    host.Start(otherEmbedding);
    PollWhileLoading(host);
    CHECK(made == 3);
    core::Settings otherSlot = otherEmbedding;
    otherSlot.serverSlot = 1;
    host.Start(otherSlot);
    PollWhileLoading(host);
    CHECK(made == 4);
    host.Start(test::EngineSettings()); // Back to llama-cpp.
    PollWhileLoading(host);
    CHECK(made == 5);
}
```

In `tests/app/AppControllerTests.cpp`, append:

```cpp
TEST_CASE("The app's backend factory builds the backend settings.json names", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_backend_kind");
    // Recording factories in place of llama.cpp and llama-server: both make a ScriptedBackend and count their calls.
    std::atomic<int> llamaCpp = 0;
    std::atomic<int> llamaServer = 0;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder("You walk."));
    app::AppController controller(test::PathsIn(dir), app::BackendFactoryByKind(
                                                          [&](const core::Settings& settings) {
                                                              ++llamaCpp;
                                                              return scripted(settings);
                                                          },
                                                          [&](const core::Settings& settings) {
                                                              ++llamaServer;
                                                              return scripted(settings);
                                                          }));
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK(llamaCpp == 1); // settings.example.json names llama-cpp.
    CHECK(llamaServer == 0);

    app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    edit.backend = core::BackendKind::LlamaServer;
    edit.chatUrl = "http://127.0.0.1:8080";
    edit.embeddingUrl = "http://127.0.0.1:8081";
    REQUIRE(controller.CanApplySettings());
    CHECK(controller.ApplySettings(edit).empty());
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK(llamaCpp == 1);
    CHECK(llamaServer == 1);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error in `tests/app/AppControllerTests.cpp`: `'BackendFactoryByKind': is not a member of 'morpheus::app'`.

Comment the new AppController test out, build, and run `./build/tests/bin/Debug/morpheus_tests.exe "[engine_host]"`.
Expected: FAIL in "EngineHost rebuilds the backend ...": `CHECK( made == 2 )` with expansion `1 == 2` (a URL change keeps the backend today). Uncomment the test.

- [ ] **Step 3: Rebuild on a backend change, and choose the factory by kind**

In `src/app/model/EngineHost.h`, replace the comment of `Start`

```cpp
    // Starts the engine with `settings`. The running worker stops first, cancelling its command and closing its
    // run. The backend is kept when the model paths, embedding prefixes, context size and GPU layers are unchanged,
    // and the engine is Ready at once. Otherwise the backend is released (freeing its memory) and a new one loads in
    // the background. Throws ContractError while Loading: a load cannot be interrupted.
```

with

```cpp
    // Starts the engine with `settings`. The running worker stops first, cancelling its command and closing its
    // run. The backend is kept when the backend kind, the model paths, the server URLs and slot, the embedding
    // prefixes, the context size and the GPU layers are unchanged, and the engine is Ready at once. Otherwise the
    // backend is released (freeing its memory) and a new one loads in the background. Throws ContractError while
    // Loading: a load cannot be interrupted.
```

and after the class's closing `};` add:

```cpp

// A factory that makes the backend settings.backend names: `llamaCpp` for BackendKind::LlamaCpp, `llamaServer` for
// BackendKind::LlamaServer. The app passes the llama.cpp and llama-server factories (main.cpp); the tests pass
// recording ones. Throws ContractError when either is empty.
EngineHost::BackendFactory BackendFactoryByKind(EngineHost::BackendFactory llamaCpp, EngineHost::BackendFactory llamaServer);
```

In `src/app/model/EngineHost.cpp`, replace

```cpp
    // Whether the loaded backend was made for the same models, prefixes and context.
    const bool sameBackend = _pBackend && settings.chatModel == _settings.chatModel &&
                             settings.embeddingModel == _settings.embeddingModel &&
                             settings.embeddingQueryPrefix == _settings.embeddingQueryPrefix &&
                             settings.embeddingDocumentPrefix == _settings.embeddingDocumentPrefix &&
                             settings.contextSize == _settings.contextSize && settings.gpuLayers == _settings.gpuLayers;
```

with

```cpp
    // Whether the loaded backend was made for the same kind of backend, models or servers, prefixes and context.
    const bool sameBackend = _pBackend && settings.backend == _settings.backend &&
                             settings.chatModel == _settings.chatModel &&
                             settings.embeddingModel == _settings.embeddingModel &&
                             settings.chatUrl == _settings.chatUrl && settings.embeddingUrl == _settings.embeddingUrl &&
                             settings.serverSlot == _settings.serverSlot &&
                             settings.embeddingQueryPrefix == _settings.embeddingQueryPrefix &&
                             settings.embeddingDocumentPrefix == _settings.embeddingDocumentPrefix &&
                             settings.contextSize == _settings.contextSize && settings.gpuLayers == _settings.gpuLayers;
```

and at the end of the file, before `} // namespace morpheus::app`, add:

```cpp
EngineHost::BackendFactory BackendFactoryByKind(EngineHost::BackendFactory llamaCpp, EngineHost::BackendFactory llamaServer)
{
    MORPHEUS_REQUIRE(llamaCpp && llamaServer, "BackendFactoryByKind: both factories are needed");
    return [llamaCpp = std::move(llamaCpp), llamaServer = std::move(llamaServer)](
               const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        switch (settings.backend) {
        case core::BackendKind::LlamaCpp:
            return llamaCpp(settings);
        case core::BackendKind::LlamaServer:
            return llamaServer(settings);
        }
        throw core::ContractError("BackendFactoryByKind: unhandled BackendKind");
    };
}

```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[engine_host],[app_controller]"`
Expected: `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 409`.

- [ ] **Step 5: Build the backend settings.json names in the app, play and probe**

In `src/app/main.cpp`, add `#include "llamaserver/LlamaServerBackend.h"` after `#include "llama/LlamaCppBackend.h"`. After `MakeLlamaBackend`, add:

```cpp

// The llama-server backend for the engine's settings: the chat and embedding servers they name. Runs on EngineHost's
// loader thread.
std::unique_ptr<inference::LlmBackend> MakeServerBackend(const core::Settings& settings)
{
    llamaserver::LlamaServerConfig config;
    config.chatUrl = settings.chatUrl;
    config.embeddingUrl = settings.embeddingUrl;
    config.slot = settings.serverSlot;
    config.contextSize = settings.contextSize;
    config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
    config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
    return std::make_unique<llamaserver::LlamaServerBackend>(config);
}
```

and replace

```cpp
        app::AppController controller(app::AppController::PathsUnder(root), MakeLlamaBackend);
```

with

```cpp
        app::AppController controller(app::AppController::PathsUnder(root),
                                      app::BackendFactoryByKind(MakeLlamaBackend, MakeServerBackend));
```

Also update `MakeLlamaBackend`'s comment line `// The llama.cpp backend for the engine's settings. Runs on EngineHost's loader thread.` to `// The llama.cpp backend for the engine's settings: the models they name. Runs on EngineHost's loader thread.`

In `src/app/CMakeLists.txt`, replace

```cmake
    # The app: the SDL3 window, ImGui's SDL backends and the llama.cpp backend.
    add_executable(morpheus main.cpp)
    target_link_libraries(morpheus PRIVATE morpheus_app_ui morpheus_llama imgui_sdl3)
```

with

```cmake
    # The app: the SDL3 window, ImGui's SDL backends, and the llama.cpp and llama-server backends.
    add_executable(morpheus main.cpp)
    target_link_libraries(morpheus PRIVATE morpheus_app_ui morpheus_llama morpheus_llamaserver imgui_sdl3)
```

In `tools/play/main.cpp`, add `#include <memory>` to the standard includes and `#include "llamaserver/LlamaServerBackend.h"` after `#include "llama/LlamaCppBackend.h"`, and replace

```cpp
        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend llamaBackend(config);
        std::optional<turn::RecordingBackend> recording;
        if (recordFile) {
            recording.emplace(llamaBackend);
        }
        inference::LlmBackend& backend =
            recording ? static_cast<inference::LlmBackend&>(*recording) : static_cast<inference::LlmBackend&>(llamaBackend);
```

with

```cpp
        // The backend settings.json names: llama.cpp in-process, or llama-server over HTTP.
        std::unique_ptr<inference::LlmBackend> pModels = nullptr;
        if (settings.backend == core::BackendKind::LlamaCpp) {
            llama::LlamaConfig config;
            config.chatModel = settings.chatModel;
            config.embeddingModel = settings.embeddingModel;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            config.contextSize = settings.contextSize;
            config.gpuLayers = settings.gpuLayers;
            pModels = std::make_unique<llama::LlamaCppBackend>(config);
        } else {
            llamaserver::LlamaServerConfig config;
            config.chatUrl = settings.chatUrl;
            config.embeddingUrl = settings.embeddingUrl;
            config.slot = settings.serverSlot;
            config.contextSize = settings.contextSize;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            pModels = std::make_unique<llamaserver::LlamaServerBackend>(config);
        }
        std::optional<turn::RecordingBackend> recording;
        if (recordFile) {
            recording.emplace(*pModels);
        }
        inference::LlmBackend& backend = recording ? static_cast<inference::LlmBackend&>(*recording) : *pModels;
```

In `tools/play/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_play PRIVATE morpheus_turn morpheus_llama)
```

with

```cmake
target_link_libraries(morpheus_play PRIVATE morpheus_turn morpheus_llama morpheus_llamaserver)
```

In `tools/probe/main.cpp`, replace the header comment's first three lines

```cpp
// morpheus_probe: loads settings.json and a world, assembles a resolve-style prompt, and streams two
// completions, printing token usage and KV-cache reuse. A manual check that the model, the Vulkan backend and
// prefix caching work on this machine.
```

with

```cpp
// morpheus_probe: loads settings.json and a world, assembles a resolve-style prompt, and streams two
// completions, printing token usage and KV-cache reuse. A manual check that the model, the GPU backend and
// prefix caching work on this machine; with "backend": "llama-server", the reuse figures are the server's
// (its timings), so the second run measures what a llama-server slot reuses.
```

Add `#include <memory>` to its standard includes and `#include "llamaserver/LlamaServerBackend.h"` after `#include "llama/LlamaCppBackend.h"`, and replace

```cpp
        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend backend(config);
```

with

```cpp
        // The backend settings.json names: llama.cpp in-process, or llama-server over HTTP.
        std::unique_ptr<inference::LlmBackend> pBackend = nullptr;
        if (settings.backend == core::BackendKind::LlamaCpp) {
            llama::LlamaConfig config;
            config.chatModel = settings.chatModel;
            config.embeddingModel = settings.embeddingModel;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            config.contextSize = settings.contextSize;
            config.gpuLayers = settings.gpuLayers;
            pBackend = std::make_unique<llama::LlamaCppBackend>(config);
        } else {
            llamaserver::LlamaServerConfig config;
            config.chatUrl = settings.chatUrl;
            config.embeddingUrl = settings.embeddingUrl;
            config.slot = settings.serverSlot;
            config.contextSize = settings.contextSize;
            config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
            config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
            pBackend = std::make_unique<llamaserver::LlamaServerBackend>(config);
            std::cout << std::format("backend: llama-server at {} (slot {})\n", settings.chatUrl, settings.serverSlot);
        }
        inference::LlmBackend& backend = *pBackend;
```

In `tools/probe/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_turn morpheus_llama)
```

with

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_turn morpheus_llama morpheus_llamaserver)
```

- [ ] **Step 6: Build the dev targets and smoke-check them**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 3`
Expected: exits with 0 (the window, the settings with `"backend": "llama-cpp"` and the font start; the models begin loading).

Run (a llama-server settings file pointing at a port where nothing listens, to see the error path):

```bash
mkdir -p build/server-smoke && perl -0pe 's/"backend": "llama-cpp"/"backend": "llama-server", "server": {"chat_url": "http:\/\/127.0.0.1:1", "embedding_url": "http:\/\/127.0.0.1:1", "slot": 0}/' settings.example.json > build/server-smoke/settings.json
./build/dev/bin/RelWithDebInfo/morpheus_probe.exe build/server-smoke/settings.json game_projects/sandcrawler "I look around."; echo "exit $?"
```

Expected: `error: cannot reach http://127.0.0.1:1/props: ...` and `exit 1` (the probe built the llama-server backend from the settings).

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes src/app/model/EngineHost.h src/app/model/EngineHost.cpp src/app/main.cpp src/app/CMakeLists.txt tools/play/main.cpp tools/play/CMakeLists.txt tools/probe/main.cpp tools/probe/CMakeLists.txt tests/app/EngineHostTests.cpp tests/app/AppControllerTests.cpp
git add src/app/model/EngineHost.h src/app/model/EngineHost.cpp src/app/main.cpp src/app/CMakeLists.txt tools/play/main.cpp tools/play/CMakeLists.txt tools/probe/main.cpp tools/probe/CMakeLists.txt tests/app/EngineHostTests.cpp tests/app/AppControllerTests.cpp
git commit -m "The app, morpheus_play and morpheus_probe build the backend settings.json names" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

