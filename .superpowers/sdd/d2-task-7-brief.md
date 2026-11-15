### Task 7: `RunRegistry`: run ids, run files and the workers by id

**Files:**
- Create: `src/server/RunRegistry.h`, `src/server/RunRegistry.cpp`
- Modify: `src/server/CMakeLists.txt` (add `RunRegistry.cpp`)
- Test: `tests/server/RunRegistryTests.cpp`

**Interfaces:**
- Consumes: `server::RunWorker` (Task 6), `server::Log`, `core::DataError`, `store::StoreError` (`src/store/Sqlite.h`), `core::PathText` (`src/core/Text.h`).
- Produces: `class server::RunRegistry` with `using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>()>;`, `static constexpr std::size_t MAX_RUN_ID_LENGTH = 64;`, `static constexpr std::string_view RUN_FILE_EXTENSION = ".morpheus";`, `RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log)`, `~RunRegistry()`, `static void CheckRunId(std::string_view runId)`, `std::filesystem::path RunFile(std::string_view runId) const`, `bool Open(const std::string& runId, turn::HostData data, std::uint64_t seed)`, `RunWorker* Find(std::string_view runId) const`, `void StopAll()`.

- [ ] **Step 1: Write the failing tests**

Create `tests/server/RunRegistryTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

#include "TestPaths.h"
#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/RunRegistry.h"
#include "server/RunWorker.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// A registry of runs in `runsDir` on scripted backends answering with `script`.
server::RunRegistry MakeRegistry(const std::filesystem::path& runsDir, test::HostedScript& script)
{
    return server::RunRegistry(
        runsDir, test::HostedSettings(),
        [&script] { return std::make_unique<test::ScriptedBackend>(16384, script.Responder()); },
        [](const std::string&) {});
}

} // namespace

TEST_CASE("RunRegistry accepts run ids of 1 to 64 letters, digits, '_' and '-'", "[hosted-server][registry]")
{
    CHECK_NOTHROW(server::RunRegistry::CheckRunId("skyrim_save-01"));
    CHECK_NOTHROW(server::RunRegistry::CheckRunId(std::string(64, 'a')));
    for (const std::string bad : {std::string(), std::string(65, 'a'), std::string("a/b"), std::string("a.b"),
                                  std::string(".."), std::string("a b")}) {
        INFO("'" << bad << "'");
        CHECK_THROWS_AS(server::RunRegistry::CheckRunId(bad), core::DataError);
        CHECK_THROWS_WITH(server::RunRegistry::CheckRunId(bad), ContainsSubstring("run_id"));
    }
}

TEST_CASE("RunRegistry creates its runs folder and names each run's file after its id", "[hosted-server][registry]")
{
    test::HostedScript script;
    const std::filesystem::path runsDir = test::FreshTempDir("registry_dir") / "nested" / "runs";
    server::RunRegistry registry = MakeRegistry(runsDir, script);
    CHECK(std::filesystem::is_directory(runsDir));
    CHECK(registry.RunFile("save_1") == runsDir / "save_1.morpheus");
    CHECK_THROWS_AS(registry.RunFile("../x"), core::DataError);

    // A runs folder that is a file cannot be made.
    const std::filesystem::path file = test::FreshTempDir("registry_file") / "runs";
    test::WriteFile(file, "not a folder");
    CHECK_THROWS_AS(MakeRegistry(file, script), core::DataError);
    CHECK_THROWS_WITH(MakeRegistry(file, script), ContainsSubstring("runs"));
}

TEST_CASE("RunRegistry opens a run once per process and reopens it on its worker; a restart finds the file",
          "[hosted-server][registry]")
{
    test::HostedScript script;
    const std::filesystem::path runsDir = test::FreshTempDir("registry_open") / "runs";
    {
        server::RunRegistry registry = MakeRegistry(runsDir, script);
        CHECK(registry.Find("save_1") == nullptr);
        CHECK(registry.Open("save_1", test::FixtureHostData(), 7));
        CHECK(std::filesystem::exists(runsDir / "save_1.morpheus"));
        server::RunWorker* pWorker = registry.Find("save_1");
        REQUIRE(pWorker != nullptr);
        CHECK(pWorker->RunId() == "save_1");
        // Again: not created, the same worker.
        CHECK_FALSE(registry.Open("save_1", test::FixtureHostData(), 7));
        CHECK(registry.Find("save_1") == pWorker);
        // The seed went to the new run.
        std::uint64_t seed = 0;
        pWorker->Call([&seed](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            seed = session.Meta().rngSeed;
        });
        CHECK(seed == 7);
    }
    // A new process: the file is there, so it is opened, not created.
    server::RunRegistry restarted = MakeRegistry(runsDir, script);
    CHECK(restarted.Find("save_1") == nullptr);
    CHECK_FALSE(restarted.Open("save_1", test::FixtureHostData(), 7));
    CHECK(restarted.Find("save_1") != nullptr);
}

TEST_CASE("RunRegistry registers nothing when a run cannot open", "[hosted-server][registry]")
{
    test::HostedScript script;
    const std::filesystem::path runsDir = test::FreshTempDir("registry_fail") / "runs";
    SECTION("bad host data")
    {
        server::RunRegistry registry = MakeRegistry(runsDir, script);
        turn::HostData bad = test::FixtureHostData();
        bad.catalog = 42;
        CHECK_THROWS_AS(registry.Open("save_1", bad, 0), core::DataError);
        CHECK(registry.Find("save_1") == nullptr);
        CHECK_FALSE(std::filesystem::exists(runsDir / "save_1.morpheus"));
    }
    SECTION("no backend")
    {
        server::RunRegistry registry(
            runsDir, test::HostedSettings(),
            []() -> std::unique_ptr<inference::LlmBackend> {
                throw inference::InferenceError("cannot reach http://127.0.0.1:1/props");
            },
            [](const std::string&) {});
        CHECK_THROWS_AS(registry.Open("save_1", test::FixtureHostData(), 0), inference::InferenceError);
        CHECK(registry.Find("save_1") == nullptr);
    }
}

TEST_CASE("RunRegistry::StopAll stops every worker: their later jobs are refused", "[hosted-server][registry]")
{
    test::HostedScript script;
    server::RunRegistry registry = MakeRegistry(test::FreshTempDir("registry_stop") / "runs", script);
    REQUIRE(registry.Open("a", test::FixtureHostData(), 0));
    REQUIRE(registry.Open("b", test::FixtureHostData(), 0));
    registry.StopAll();
    for (const char* runId : {"a", "b"}) {
        server::RunWorker* pWorker = registry.Find(runId);
        REQUIRE(pWorker != nullptr);
        CHECK_THROWS_WITH(pWorker->Call([](turn::HostedSession&, server::SlotBackend&, std::stop_token) {}),
                          ContainsSubstring("the server is stopping"));
    }
}
```

`RunRegistry` holds mutexes, so it is not movable; `MakeRegistry` returning by value relies on guaranteed copy elision (C++17), which holds for a prvalue `return server::RunRegistry(...)`. If the compiler still objects, change `MakeRegistry` to return `std::unique_ptr<server::RunRegistry>` and adapt the tests.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/RunRegistry.h'`.

- [ ] **Step 3: Write `RunRegistry`**

Create `src/server/RunRegistry.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "core/Settings.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/ServerLog.h"
#include "turn/HostedWorld.h"

namespace morpheus::server {

// The runs morpheus_server has opened (spec, section 2): each run id's file in the runs folder and its worker. A run
// stays open until the registry is destroyed. Thread-safe.
class RunRegistry {
public:
    // Makes each run's own backend (backends are not thread-safe, so runs do not share one).
    using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>()>;

    static constexpr std::size_t MAX_RUN_ID_LENGTH = 64;
    static constexpr std::string_view RUN_FILE_EXTENSION = ".morpheus";

private:
    std::filesystem::path _runsDir = {};
    core::Settings _settings = {};
    BackendFactory _makeBackend = {};
    Log _log = {};
    std::mutex _openMutex = {};     // One open at a time: two opens of one new run make one worker.
    mutable std::mutex _mutex = {}; // Guards _runs.
    std::map<std::string, std::unique_ptr<RunWorker>, std::less<>> _runs = {};

public:
    // Creates `runsDir` when it is missing; DataError naming it when it cannot. Throws ContractError for an empty
    // factory or log.
    RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log);
    // Stops every worker (StopAll), then joins them.
    ~RunRegistry();
    RunRegistry(const RunRegistry&) = delete;
    RunRegistry& operator=(const RunRegistry&) = delete;

    // Throws DataError "run_id: ..." unless `runId` is 1 to MAX_RUN_ID_LENGTH characters of A-Z, a-z, 0-9, '_' and '-'.
    static void CheckRunId(std::string_view runId);

    // "<runs dir>/<run_id>.morpheus". Checks the id first (CheckRunId).
    std::filesystem::path RunFile(std::string_view runId) const;

    // Opens run `runId` with `data`. The first open in this process makes its worker, which creates the file (with
    // `seed` for sampling) or reopens it; a later open reopens it on its worker (RunWorker::Reopen). Returns whether the
    // file was created. Throws what the open throws (DataError, StoreError, InferenceError, including the backend
    // factory's); nothing is registered then. Opens take turns, so a long open (lore to embed) delays other opens, not
    // other runs' requests.
    bool Open(const std::string& runId, turn::HostData data, std::uint64_t seed);

    // The worker of a run opened in this process, or nullptr. It lives until the registry is destroyed.
    RunWorker* Find(std::string_view runId) const;

    // Stops every worker without waiting (RunWorker::RequestStop): running turns and folds stop, and later jobs fail.
    void StopAll();
};

} // namespace morpheus::server
```

Create `src/server/RunRegistry.cpp`:

```cpp
#include "server/RunRegistry.h"

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "store/Sqlite.h"

namespace morpheus::server {

RunRegistry::RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log)
    : _runsDir(std::move(runsDir))
    , _settings(std::move(settings))
    , _makeBackend(std::move(makeBackend))
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_makeBackend) && static_cast<bool>(_log),
                     "RunRegistry: a backend factory and a log are needed");
    // The runs folder is the operator's choice (--runs): one that cannot be made is a configuration error.
    std::error_code error;
    std::filesystem::create_directories(_runsDir, error);
    if (error || !std::filesystem::is_directory(_runsDir)) {
        throw core::DataError(std::format("runs folder '{}': {}", core::PathText(_runsDir),
                                          error ? error.message() : std::string("it is not a folder")));
    }
}

RunRegistry::~RunRegistry()
{
    // Every worker stops at once; then _runs' destruction joins them one by one.
    StopAll();
}

void RunRegistry::CheckRunId(std::string_view runId)
{
    const bool allowed = std::all_of(runId.begin(), runId.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    if (runId.empty() || runId.size() > MAX_RUN_ID_LENGTH || !allowed) {
        throw core::DataError(std::format("run_id: '{}' is not 1 to {} characters of A-Z, a-z, 0-9, '_' and '-'", runId,
                                          MAX_RUN_ID_LENGTH));
    }
}

std::filesystem::path RunRegistry::RunFile(std::string_view runId) const
{
    CheckRunId(runId);
    return _runsDir / (std::string(runId) + std::string(RUN_FILE_EXTENSION));
}

bool RunRegistry::Open(const std::string& runId, turn::HostData data, std::uint64_t seed)
{
    const std::filesystem::path file = RunFile(runId);
    std::scoped_lock opening(_openMutex);

    // A run opened in this process reopens on its own worker, after its queued jobs.
    if (RunWorker* pWorker = Find(runId); pWorker != nullptr) {
        pWorker->Reopen(std::move(data), seed);
        return false;
    }

    // Otherwise it gets a worker, which opens the file or creates it. Nothing is registered when that throws.
    std::error_code error;
    const bool existed = std::filesystem::exists(file, error);
    if (error) {
        throw store::StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }
    auto pWorker = std::make_unique<RunWorker>(runId, file, std::move(data), _settings, seed, _makeBackend(), _log);
    std::scoped_lock lock(_mutex);
    _runs.emplace(runId, std::move(pWorker));
    return !existed;
}

RunWorker* RunRegistry::Find(std::string_view runId) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _runs.find(runId);
    return it == _runs.end() ? nullptr : it->second.get();
}

void RunRegistry::StopAll()
{
    std::scoped_lock lock(_mutex);
    for (auto& [runId, pWorker] : _runs) {
        pWorker->RequestStop();
    }
}

} // namespace morpheus::server
```

Add `RunRegistry.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[registry]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/server/RunRegistry.h src/server/RunRegistry.cpp src/server/CMakeLists.txt tests/server/RunRegistryTests.cpp
git commit -m "RunRegistry: run ids, run files, and one worker per opened run"
```

---

