#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

#include "LlamaTestModels.h"
#include "TestEnvironment.h"
#include "inference/Llm.h"
#include "llamaserver/LiveServer.h"
#include "llamaserver/LlamaServerBackend.h"

using namespace morpheus;

TEST_CASE("llama-server and the in-process backend count the same tokens", "[server][llama]")
{
    llama::LlamaCppBackend& local = test::SharedBackend();
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(0);

    // The same model on both sides: the server's model file must be MORPHEUS_TEST_CHAT_MODEL's.
    httplib::Client client(config.chatUrl);
    const httplib::Result props = client.Get("/props");
    REQUIRE(props);
    REQUIRE(props->status == 200);
    const std::string serverModel =
        std::filesystem::path(nlohmann::json::parse(props->body).at("model_path").get<std::string>()).filename().string();
    const std::string localModel =
        std::filesystem::path(test::EnvironmentVariable("MORPHEUS_TEST_CHAT_MODEL")).filename().string();
    if (serverModel != localModel) {
        SKIP("the chat server runs " + serverModel + ", not MORPHEUS_TEST_CHAT_MODEL's " + localModel);
    }
    llamaserver::LlamaServerBackend server(config);

    for (const std::string text : {"", "The crawler groans over the dunes.", "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC",
                                   R"({"intent":"climb","checks":[]})", "  leading spaces and\nnew lines\n"}) {
        INFO(text);
        CHECK(server.CountTokens(text) == local.CountTokens(text));
    }
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::System, "You are the game master."},
                       {inference::Role::User, "I climb the dune."},
                       {inference::Role::Assistant, R"({"intent":"climb","checks":[],"changes":[]})"},
                       {inference::Role::User, "Narrate it."}};
    CHECK(server.CountPromptTokens(prompt) == local.CountPromptTokens(prompt));
}
