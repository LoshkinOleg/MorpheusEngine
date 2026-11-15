### Task 11: The `morpheus_server` executable, and the live test

**Files:**
- Create: `tools/server/CMakeLists.txt`, `tools/server/main.cpp`
- Modify: `CMakeLists.txt` (add `tools/server` beside `src/server`)
- Test: `tests/server/ServerLiveTests.cpp`

**Interfaces:**
- Consumes: everything in `src/server` (Tasks 4-10), `core::LoadSettings`, `core::BackendKindName` (`src/core/Settings.h`), `llamaserver::LlamaServerBackend`, `test::LiveServerConfig` (`tests/llamaserver/LiveServer.h`), the Task 10 helpers.
- Produces: the executable `morpheus_server [--port N] [--runs DIR] [--settings FILE]`; exit code 0 after a signal, 1 for a startup failure, 2 for bad flags. The `[server]` test `"morpheus_server against real llama-servers: ..."`.

- [ ] **Step 1: Write the live test**

Create `tests/server/ServerLiveTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "hosted/HostedTestSupport.h"
#include "llamaserver/LiveServer.h"
#include "llamaserver/LlamaServerBackend.h"
#include "server/Api.h"
#include "server/HttpServer.h"
#include "server/RunRegistry.h"
#include "server/ServerTestSupport.h"
#include "server/SlotPool.h"

using namespace morpheus;

TEST_CASE("morpheus_server against real llama-servers: the conversation fixture over HTTP, and the time to each "
          "turn's first sentence",
          "[server]")
{
    // The owner's servers (README, "The [server] tests"); skipped when the variables are unset.
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(server::SlotPool::UPKEEP_SLOT);
    llamaserver::LlamaServerBackend utility(config);
    server::SlotPool slots(utility.SlotCount());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = test::LIVE_CONTEXT_SIZE;
    server::RunRegistry runs(test::FreshTempDir("server_live") / "runs", settings,
                             [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); },
                             [](const std::string&) {});
    server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                    [](const std::string&) {});
    server::HttpServer http(api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    client.set_read_timeout(std::chrono::seconds(600));

    REQUIRE(test::JsonPost(client, "/v1/runs/live/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture("conversation.json");
    const test::HttpJson started =
        test::JsonPost(client, "/v1/runs/live/conversations",
                       {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}});
    REQUIRE(started.status == 200);
    const std::string base = "/v1/runs/live/conversations/" + started.body.at("conversation_id").get<std::string>();

    // The fixture's requests, answered by the real model. The first turn fills the slot's cache; later turns show the
    // warm figure the host's 1.5 s target is about (measured here, not asserted: the target is for its RTX 3060 Ti).
    int turnNumber = 0;
    for (const nlohmann::json& step : fixture.at("steps")) {
        if (step.contains("events")) {
            REQUIRE(test::JsonPost(client, base + "/events", {{"events", step.at("events")}}).status == 200);
            continue;
        }
        nlohmann::json body = step.at("turn");
        if (!body.contains("observation")) {
            body["observation"] = test::FixtureObservation();
        }
        const auto sent = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::duration> firstSentence;
        const test::HttpTurn turn = test::PostTurn(client, base + "/turn", body, [&](const server::StreamEvent& event) {
            if (event.name == "sentence" && !firstSentence) {
                firstSentence = std::chrono::steady_clock::now() - sent;
            }
            return true;
        });
        ++turnNumber;
        INFO("turn " << turnNumber);
        REQUIRE(turn.status == 200);
        REQUIRE_FALSE(turn.events.empty());
        CHECK(turn.events.front().name == "action");
        CHECK(turn.events.back().name == "done");
        const nlohmann::json& done = turn.events.back().data;
        WARN(std::format("turn {}: first sentence after {} ms; action {}; line: {}", turnNumber,
                         firstSentence ? std::chrono::duration_cast<std::chrono::milliseconds>(*firstSentence).count() : -1,
                         done.at("action").dump(), done.at("line").get<std::string>()));
    }
    runs.StopAll();
    http.Stop();
}
```

- [ ] **Step 2: Run it to see it compile and skip**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[server]"`
Expected (without the `MORPHEUS_TEST_*_URL` variables): every `[server]` test is skipped, this one included.

- [ ] **Step 3: Write the executable**

Create `tools/server/CMakeLists.txt`:

```cmake
add_executable(morpheus_server main.cpp)
target_link_libraries(morpheus_server PRIVATE morpheus_server_lib)
morpheus_target_defaults(morpheus_server)
```

In `CMakeLists.txt`, change the server block to:

```cmake
if(MORPHEUS_BUILD_SERVER)
    add_subdirectory(src/server)
    add_subdirectory(tools/server)
endif()
```

Create `tools/server/main.cpp`:

```cpp
// morpheus_server: the HTTP server for hosted runs (docs/superpowers/specs/2026-11-16-morpheus-server-design.md).
//   morpheus_server [--port N] [--runs DIR] [--settings FILE]
// It reads settings.json (which must name the llama-server backend), checks both llama-servers, and serves
// http://127.0.0.1:N until Ctrl+C or SIGTERM.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "core/Text.h"
#include "llamaserver/LlamaServerBackend.h"
#include "server/Api.h"
#include "server/HttpServer.h"
#include "server/RunRegistry.h"
#include "server/ServerLog.h"
#include "server/SlotPool.h"

using namespace morpheus;

namespace {

constexpr int DEFAULT_PORT = 4998;
constexpr const char* DEFAULT_RUNS = "runs";
constexpr const char* DEFAULT_SETTINGS = "settings.json";
constexpr const char* USAGE = "usage: morpheus_server [--port N] [--runs DIR] [--settings FILE]\n";
constexpr int MAX_PORT = 65535;
constexpr std::chrono::milliseconds SIGNAL_POLL = std::chrono::milliseconds(100);

// Set by the signal handler, polled by main. A lock-free atomic may be set from a handler.
std::atomic<bool> stopRequested = false;

extern "C" void OnSignal(int)
{
    stopRequested = true;
}

} // namespace

int main(int argc, char** argv)
{
    // The flags, each with a value.
    int port = DEFAULT_PORT;
    std::filesystem::path runsDir = DEFAULT_RUNS;
    std::filesystem::path settingsFile = DEFAULT_SETTINGS;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc || (flag != "--port" && flag != "--runs" && flag != "--settings")) {
            std::cerr << USAGE;
            return 2;
        }
        const std::string value = argv[++i];
        if (flag == "--port") {
            // Digits only, from 1 to 65535.
            std::size_t used = 0;
            try {
                port = std::stoi(value, &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != value.size() || port < 1 || port > MAX_PORT) {
                std::cerr << "morpheus_server: --port needs a number from 1 to 65535\n" << USAGE;
                return 2;
            }
        } else if (flag == "--runs") {
            runsDir = value;
        } else {
            settingsFile = value;
        }
    }

    // One logger for every thread: a line at a time on stderr.
    std::mutex logMutex;
    const server::Log log = [&logMutex](const std::string& line) {
        std::scoped_lock lock(logMutex);
        std::cerr << "morpheus_server: " << line << '\n';
    };

    try {
        const core::Settings settings = core::LoadSettings(settingsFile);
        if (settings.backend != core::BackendKind::LlamaServer) {
            log(std::format("{}: the backend is \"{}\"; morpheus_server needs \"backend\": \"{}\"",
                            core::PathText(settingsFile), core::BackendKindName(settings.backend),
                            core::BackendKindName(core::BackendKind::LlamaServer)));
            return 1;
        }

        // Every backend talks to the same servers; a call left on the default slot goes to the upkeep slot.
        // settings.json's server slot is for Morpheus's own game: the server hands out slots itself.
        llamaserver::LlamaServerConfig config;
        config.chatUrl = settings.chatUrl;
        config.embeddingUrl = settings.embeddingUrl;
        config.slot = server::SlotPool::UPKEEP_SLOT;
        config.contextSize = settings.contextSize;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;

        // The utility backend checks both servers now, serves /v1/tokenize and /v1/embeddings, and tells the slot
        // count and the model ids.
        llamaserver::LlamaServerBackend utility(config);
        server::SlotPool slots(utility.SlotCount());
        if (utility.SlotCount() == 1) {
            log("the chat server has one slot: folding and turns share it, so their cached prompts evict each other "
                "(start llama-server with --parallel 3 or more)");
        }
        server::RunRegistry runs(runsDir, settings,
                                 [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); }, log);
        server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                        log);
        server::HttpServer http(api);
        http.Start(port);
        log(std::format("listening on http://127.0.0.1:{}; runs in '{}'; chat model {} ({} slots), embedding model {}",
                        http.Port(), core::PathText(std::filesystem::absolute(runsDir)), utility.ChatModelId(),
                        utility.SlotCount(), utility.EmbeddingModelId()));

        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
        while (!stopRequested) {
            std::this_thread::sleep_for(SIGNAL_POLL);
        }

        // Running turns and folds stop first, so the HTTP handlers waiting on them return; then the server stops,
        // and the workers join as `runs` goes.
        log("stopping");
        runs.StopAll();
        http.Stop();
        return 0;
    } catch (const std::exception& error) {
        log(error.what());
        return 1;
    }
}
```

- [ ] **Step 4: Build it in each preset and run it**

Run: `cmake --build --preset tests`, then `cmake --build --preset dev --target morpheus_server` (configure `dev` first if needed: `cmake --preset dev`).
Expected: both build with no warning.

Then, with no settings file: `./build/tests/bin/Debug/morpheus_server.exe --settings nowhere.json`
Expected: exit code 1 and a message naming `nowhere.json`. Bad flags: `./build/tests/bin/Debug/morpheus_server.exe --port 0` gives exit code 2 and the usage line.

If llama-servers are running on this machine (README, "Models through llama-server") and `settings.json` names the llama-server backend: start `./build/tests/bin/Debug/morpheus_server.exe` in the background (Bash `run_in_background`), then `curl -s http://127.0.0.1:4998/health`. Expected: JSON with `"ready":true` and `"api_version":"1.0.0"`. Then stop it (TaskStop) and check `tasklist | grep -i morpheus` shows nothing. If no llama-server is running, skip this check and say so in the report.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tools/server/ tests/server/ServerLiveTests.cpp
git commit -m "morpheus_server: the executable (flags, settings, both servers checked, signals) and its live test"
```

---

