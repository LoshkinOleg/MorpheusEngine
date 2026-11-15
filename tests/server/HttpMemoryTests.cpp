#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/HttpServer.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;

TEST_CASE("The three-actor fixture plays over HTTP", "[hosted-server][http][memory][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("three_actors.json");
    test::HostedScript script = test::ScriptOf(fixture);
    test::ApiHarness harness("http_memory_fixture", test::MemoryEchoResponder(script));
    server::HttpServer http(harness.api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    test::PlayMemoryFixtureOverHttp(client, "r", fixture, true);
    harness.runs.StopAll();
    http.Stop();
}

TEST_CASE("The reload fixture plays over HTTP", "[hosted-server][http][rewind][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("reload.json");
    test::HostedScript script = test::ScriptOf(fixture);
    test::ApiHarness harness("http_reload_fixture", test::MemoryEchoResponder(script));
    server::HttpServer http(harness.api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    test::PlayMemoryFixtureOverHttp(client, "r", fixture, true);
    harness.runs.StopAll();
    http.Stop();
}

TEST_CASE("The prompt-pack fixture plays over HTTP", "[hosted-server][http][prompts][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("prompt_pack.json");
    test::HostedScript script = test::ScriptOf(fixture);
    test::ApiHarness harness("http_prompt_pack_fixture", test::MemoryEchoResponder(script));
    server::HttpServer http(harness.api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    test::PlayMemoryFixtureOverHttp(client, "r", fixture, true);
    harness.runs.StopAll();
    http.Stop();
}
