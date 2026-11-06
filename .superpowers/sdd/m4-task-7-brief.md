### Task 7: The engine host

`EngineHost` owns the backend and the worker. The llama.cpp backend takes seconds to load its models, so it is made on a background thread while the UI keeps drawing. A restart that changes only sampling or memory mode keeps the loaded backend and replaces only the worker. A restart that needs new models releases the old ones first, since both may not fit in 8 GB of VRAM.

**Files:**
- Create: `src/app/model/EngineHost.h`, `src/app/model/EngineHost.cpp`, `tests/app/EngineHostTests.cpp`
- Modify: `src/app/CMakeLists.txt` (add `model/EngineHost.cpp`), `tests/app/AppTestSupport.h` (add `EngineSettings`)

**Interfaces:**
- Consumes: `turn::Worker(core::Settings, inference::LlmBackend&)`, `core::Settings`.
- Produces: `class app::EngineHost` with:
  - the types `using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>(const core::Settings&)>` and `enum class Status { Stopped, Loading, Ready, Failed }`;
  - `explicit EngineHost(BackendFactory)`, `void Start(core::Settings)` and `void Poll()`;
  - `Status CurrentStatus() const`, `const std::string& Failure() const`, `const core::Settings& CurrentSettings() const` and `turn::Worker* ActiveWorker()`.
  - Test support: `test::EngineSettings()`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/app/AppTestSupport.h` (include `"core/Settings.h"`):

```cpp
// Settings for engines on a ScriptedBackend: the budgets the worker tests use, and model paths that are never opened.
inline core::Settings EngineSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf";
    settings.embeddingModel = "models/embed.gguf";
    settings.contextSize = 16384;
    settings.budget = {256, 256, 256, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}
```

Create `tests/app/EngineHostTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <variant>

#include "TestPaths.h"
#include "app/AppTestSupport.h"
#include "app/model/EngineHost.h"
#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// Polls `host` until it leaves Loading. Fails the test after 10 s.
void PollWhileLoading(app::EngineHost& host)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (host.CurrentStatus() == app::EngineHost::Status::Loading) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        host.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// A factory that makes ScriptedBackends and counts them.
app::EngineHost::BackendFactory CountingFactory(std::atomic<int>& made)
{
    return [&made](const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        ++made;
        return std::make_unique<test::ScriptedBackend>(settings.contextSize);
    };
}

} // namespace

TEST_CASE("EngineHost loads the backend in the background, then runs a worker", "[engine_host]")
{
    std::atomic<int> made = 0;
    app::EngineHost host(CountingFactory(made));
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Stopped);
    CHECK(host.ActiveWorker() == nullptr);

    host.Start(test::EngineSettings());
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Loading);
    CHECK(host.ActiveWorker() == nullptr);
    PollWhileLoading(host);
    REQUIRE(host.CurrentStatus() == app::EngineHost::Status::Ready);
    CHECK(made == 1);
    REQUIRE(host.ActiveWorker() != nullptr);

    // The worker runs commands on the loaded backend.
    host.ActiveWorker()->Post(
        turn::StartRun{test::TestDataDir() / "worlds" / "mini", test::FreshTempDir("engine_host") / "run.db", 7});
    const std::optional<turn::Event> opened = host.ActiveWorker()->WaitEvent(std::chrono::seconds(10));
    REQUIRE(opened);
    CHECK(std::holds_alternative<turn::RunOpened>(*opened));
}

TEST_CASE("EngineHost keeps the backend when only sampling or memory mode change", "[engine_host]")
{
    std::atomic<int> made = 0;
    app::EngineHost host(CountingFactory(made));
    host.Start(test::EngineSettings());
    PollWhileLoading(host);

    core::Settings tuned = test::EngineSettings();
    tuned.narrateSampling.temperature = 1.2;
    tuned.memoryMode = core::MemoryMode::Assisted;
    host.Start(tuned);
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Ready); // At once: nothing to load.
    CHECK(made == 1);
    CHECK(host.CurrentSettings().narrateSampling.temperature == 1.2);
    CHECK(host.ActiveWorker() != nullptr);

    core::Settings bigger = tuned;
    bigger.contextSize = 32768;
    host.Start(bigger);
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Loading);
    PollWhileLoading(host);
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Ready);
    CHECK(made == 2);
}

TEST_CASE("EngineHost reports a backend that fails to load", "[engine_host]")
{
    app::EngineHost host([](const core::Settings&) -> std::unique_ptr<inference::LlmBackend> {
        throw inference::InferenceError("cannot load model 'chat.gguf'");
    });
    host.Start(test::EngineSettings());
    PollWhileLoading(host);
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Failed);
    CHECK_THAT(host.Failure(), ContainsSubstring("chat.gguf"));
    CHECK(host.ActiveWorker() == nullptr);
}

TEST_CASE("EngineHost refuses a restart while models load", "[engine_host]")
{
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    app::EngineHost host([released](const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        released.wait(); // Loading lasts until the test releases it.
        return std::make_unique<test::ScriptedBackend>(settings.contextSize);
    });
    host.Start(test::EngineSettings());
    CHECK_THROWS_AS(host.Start(test::EngineSettings()), core::ContractError);
    release.set_value();
    PollWhileLoading(host);
    CHECK(host.CurrentStatus() == app::EngineHost::Status::Ready);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Add `model/EngineHost.cpp` to `src/app/CMakeLists.txt`.

Run: `cmake --build --preset tests`
Expected: a compile error: `app/model/EngineHost.h` is missing.

- [ ] **Step 3: Write the header**

Create `src/app/model/EngineHost.h`:

```cpp
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "inference/Llm.h"
#include "turn/Worker.h"

namespace morpheus::app {

// Owns the backend and the worker (UI thread only). Models load on a background thread, so the UI keeps drawing.
// A restart that changes no backend setting keeps the loaded backend and replaces only the worker.
class EngineHost {
public:
    // Makes the backend for `settings`. It may take seconds (it loads models) and may throw.
    using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>(const core::Settings&)>;

    enum class Status { Stopped, Loading, Ready, Failed };

private:
    BackendFactory _factory = {};
    core::Settings _settings = {};
    Status _status = Status::Stopped;
    std::string _failure = {};
    std::unique_ptr<inference::LlmBackend> _pBackend = nullptr;
    std::unique_ptr<turn::Worker> _pWorker = nullptr; // After the backend, so it is destroyed first.
    std::mutex _loadMutex = {};
    std::unique_ptr<inference::LlmBackend> _pLoaded = nullptr; // The loader's result, under _loadMutex.
    std::string _loadError = {};                               // The loader's failure, under _loadMutex.
    bool _loadDone = false;                                    // Under _loadMutex.
    std::jthread _loader = {}; // Last: joined first on destruction, before the members it writes.

public:
    explicit EngineHost(BackendFactory factory);

    // Starts the engine with `settings`. The running worker stops first, cancelling its command and closing its
    // run. The backend is kept when the model paths, embedding prefixes, context size and GPU layers are unchanged,
    // and the engine is Ready at once. Otherwise the backend is released (freeing its memory) and a new one loads in
    // the background. Throws ContractError while Loading: a load cannot be interrupted.
    void Start(core::Settings settings);

    // Takes a finished load: Ready with a new worker, or Failed. Call once per frame.
    void Poll();

    Status CurrentStatus() const;
    const std::string& Failure() const;             // Why the last load failed (Failed).
    const core::Settings& CurrentSettings() const;  // The settings of the last Start.
    turn::Worker* ActiveWorker();                   // The worker while Ready, else nullptr.
};

} // namespace morpheus::app
```

- [ ] **Step 4: Write the implementation**

Create `src/app/model/EngineHost.cpp`:

```cpp
#include "app/model/EngineHost.h"

#include <exception>
#include <utility>

#include "core/Errors.h"

namespace morpheus::app {

EngineHost::EngineHost(BackendFactory factory)
    : _factory(std::move(factory))
{
}

void EngineHost::Start(core::Settings settings)
{
    MORPHEUS_REQUIRE(_status != Status::Loading, "EngineHost: Start while models load");
    // Whether the loaded backend was made for the same models, prefixes and context.
    const bool sameBackend = _pBackend && settings.chatModel == _settings.chatModel &&
                             settings.embeddingModel == _settings.embeddingModel &&
                             settings.embeddingQueryPrefix == _settings.embeddingQueryPrefix &&
                             settings.embeddingDocumentPrefix == _settings.embeddingDocumentPrefix &&
                             settings.contextSize == _settings.contextSize && settings.gpuLayers == _settings.gpuLayers;
    _pWorker.reset(); // Cancels its command and joins it; the run it had open closes.
    _settings = std::move(settings);
    _failure.clear();
    if (sameBackend) {
        _pWorker = std::make_unique<turn::Worker>(_settings, *_pBackend);
        _status = Status::Ready;
        return;
    }

    // Free the old models before loading new ones: both may not fit in GPU memory.
    _pBackend.reset();
    {
        std::scoped_lock lock(_loadMutex);
        _pLoaded.reset();
        _loadError.clear();
        _loadDone = false;
    }
    _status = Status::Loading;
    // The loader only calls the factory and hands its result over under the lock; Poll takes it.
    _loader = std::jthread([this, settings = _settings] {
        std::unique_ptr<inference::LlmBackend> pLoaded = nullptr;
        std::string error;
        try {
            pLoaded = _factory(settings);
            MORPHEUS_REQUIRE(pLoaded != nullptr, "EngineHost: the backend factory returned no backend");
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        std::scoped_lock lock(_loadMutex);
        _pLoaded = std::move(pLoaded);
        _loadError = std::move(error);
        _loadDone = true;
    });
}

void EngineHost::Poll()
{
    if (_status != Status::Loading) {
        return;
    }
    std::unique_ptr<inference::LlmBackend> pLoaded = nullptr;
    std::string error;
    {
        std::scoped_lock lock(_loadMutex);
        if (!_loadDone) {
            return; // Still loading.
        }
        pLoaded = std::move(_pLoaded);
        error = std::move(_loadError);
    }
    _loader.join(); // The loader has finished its work: this returns at once.
    if (!pLoaded) {
        _status = Status::Failed;
        _failure = std::move(error);
        return;
    }
    _pBackend = std::move(pLoaded);
    _pWorker = std::make_unique<turn::Worker>(_settings, *_pBackend);
    _status = Status::Ready;
}

EngineHost::Status EngineHost::CurrentStatus() const
{
    return _status;
}

const std::string& EngineHost::Failure() const
{
    return _failure;
}

const core::Settings& EngineHost::CurrentSettings() const
{
    return _settings;
}

turn::Worker* EngineHost::ActiveWorker()
{
    return _status == Status::Ready ? _pWorker.get() : nullptr;
}

} // namespace morpheus::app
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[engine_host]"`
Expected: all four tests pass. Then `ctest --preset tests`: everything passes.

- [ ] **Step 6: Commit**

```bash
git add src/app/CMakeLists.txt src/app/model/EngineHost.h src/app/model/EngineHost.cpp tests/app/AppTestSupport.h tests/app/EngineHostTests.cpp
git commit -m "Load the engine's backend in the background and keep it across restarts" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

