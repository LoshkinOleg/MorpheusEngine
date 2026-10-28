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
