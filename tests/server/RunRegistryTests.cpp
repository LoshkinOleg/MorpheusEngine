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

TEST_CASE("RunRegistry refuses Windows device names as run ids, in any case", "[hosted-server][registry]")
{
    for (const std::string bad : {"CON", "con", "Prn", "AUX", "nul", "COM1", "com9", "LPT1", "lpt9"}) {
        INFO(bad);
        CHECK_THROWS_AS(server::RunRegistry::CheckRunId(bad), core::DataError);
        CHECK_THROWS_WITH(server::RunRegistry::CheckRunId(bad), ContainsSubstring("run_id"));
    }
    // Only the names themselves: longer ids that start with one are fine.
    for (const std::string good : {"CON1", "console", "COM0", "COM10", "LPT", "nul_save", "aux-2"}) {
        INFO(good);
        CHECK_NOTHROW(server::RunRegistry::CheckRunId(good));
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

TEST_CASE("RunRegistry refuses a run id that differs from an opened run's only in case", "[hosted-server][registry]")
{
    test::HostedScript script;
    server::RunRegistry registry = MakeRegistry(test::FreshTempDir("registry_case") / "runs", script);
    REQUIRE(registry.Open("save_1", test::FixtureHostData(), 0));
    CHECK_THROWS_AS(registry.Open("SAVE_1", test::FixtureHostData(), 0), core::DataError);
    CHECK_THROWS_WITH(registry.Open("SAVE_1", test::FixtureHostData(), 0),
                      ContainsSubstring("SAVE_1") && ContainsSubstring("save_1") && ContainsSubstring("case"));
    CHECK(registry.Find("SAVE_1") == nullptr);
    // The exact id still reopens.
    CHECK_FALSE(registry.Open("save_1", test::FixtureHostData(), 0));
}

TEST_CASE("RunRegistry refuses to open a run after StopAll", "[hosted-server][registry]")
{
    test::HostedScript script;
    server::RunRegistry registry = MakeRegistry(test::FreshTempDir("registry_stopped_open") / "runs", script);
    registry.StopAll();
    CHECK_THROWS_AS(registry.Open("late", test::FixtureHostData(), 0), inference::InferenceError);
    CHECK_THROWS_WITH(registry.Open("late", test::FixtureHostData(), 0), ContainsSubstring("the server is stopping"));
    CHECK(registry.Find("late") == nullptr);
}
