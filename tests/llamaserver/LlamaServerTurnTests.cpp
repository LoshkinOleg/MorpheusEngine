#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/FakeLlamaServer.h"
#include "hosted/HostedTestSupport.h"
#include "llamaserver/LlamaServerBackend.h"
#include "llamaserver/ServerTestSupport.h"
#include "turn/HostedSession.h"
#include "turn/Session.h"

using namespace morpheus;

namespace {

// `text` in pieces of 4 bytes, as the fake streams a completion.
test::FakeLlamaServer::CompletionScript Streamed(const std::string& text)
{
    test::FakeLlamaServer::CompletionScript script;
    for (std::size_t at = 0; at < text.size(); at += 4) {
        script.pieces.push_back(text.substr(at, 4));
    }
    return script;
}

} // namespace

TEST_CASE("Turns play through llama-server when its chat template refuses a prompt without a user message",
          "[llamaserver]")
{
    // Qwen3.5's template, as llama-server applies it: every message list without a user message is refused.
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.RefuseWithoutUserMessage(true);

    SECTION("an owned turn")
    {
        llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
        core::Settings settings;
        settings.backend = core::BackendKind::LlamaServer;
        settings.chatUrl = chat.Url();
        settings.embeddingUrl = embed.Url();
        settings.serverSlot = 1;
        settings.embeddingQueryPrefix = "search_query: ";
        settings.embeddingDocumentPrefix = "search_document: ";
        settings.contextSize = 8192;
        settings.budget = {256, 256, 256, 1024, 1024, 1024, 512, 256};
        settings.resolveSampling = {0.3, 40, 0.9};
        settings.narrateSampling = {0.8, 40, 0.95};
        turn::Session session =
            turn::Session::Start(core::LoadWorld(test::TestDataDir() / "worlds" / "mini"), settings,
                                 test::FreshTempDir("llamaserver_owned_turn") / "run.db", 42, backend);

        chat.QueueCompletion(Streamed(R"({"intent":"walk","checks":[],"changes":[]})"));
        chat.QueueCompletion(Streamed("You walk on."));
        const std::optional<core::TurnRecord> record = session.RunTurn("I walk.", {}, std::stop_token());
        REQUIRE(record.has_value());
        CHECK(record->narration == "You walk on.");
    }
    SECTION("a hosted turn")
    {
        llamaserver::LlamaServerConfig config = test::ConfigFor(chat, embed);
        config.contextSize = 16384;
        llamaserver::LlamaServerBackend backend(config);
        turn::HostedSession session =
            turn::HostedSession::Open(test::HostedRunFile("llamaserver_hosted_turn"), test::FixtureHostData(),
                                      test::HostedSettings(), 42, backend);
        const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

        chat.QueueCompletion(Streamed(R"({"action":"none","params":{}})"));
        chat.QueueCompletion(Streamed("Welcome."));
        const std::optional<core::HostedTurn> record =
            session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, std::stop_token());
        REQUIRE(record.has_value());
        CHECK(record->line == "Welcome.");
    }
    // No prompt was refused.
    for (const test::FakeLlamaServer::Request& request : chat.RequestsTo("/apply-template")) {
        bool hasUser = false;
        for (const nlohmann::json& message : request.body.at("messages")) {
            hasUser = hasUser || message.at("role") == "user";
        }
        CHECK(hasUser);
    }
}
