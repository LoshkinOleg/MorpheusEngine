#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "hosted/HostedTestSupport.h"
#include "llama/LlamaTestModels.h"
#include "turn/HostedSession.h"

using namespace morpheus;

TEST_CASE("A hosted conversation on the real model: every action validates and every line is spoken", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // Budgets for the shared backend's context of 4096, greedy sampling, no thinking.
    // Fields in order: resolveMaxTokens, narrateMaxTokens, narrateAllowance, tailAllowance, engineRulesCap, worldCap,
    // summaryCap, retrievalCap.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.0, 40, 0.95};
    settings.narrateSampling = {0.0, 40, 0.95};
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("llama_hosted"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    const std::vector<std::string> playerTexts = {"Hulda, I need a room for the night.",
                                                  "Saadia keeps staring at us. Should I worry?",
                                                  "Then follow me, we are leaving."};
    for (const std::string& text : playerTexts) {
        INFO("player: " << text);
        std::string streamed;
        turn::HostedCallbacks callbacks;
        callbacks.onToken = [&streamed](std::string_view piece) { streamed += piece; };
        const std::optional<core::HostedTurn> record = session.RunTurn(test::Request(id, test::HULDA, text), callbacks, {});
        REQUIRE(record);
        INFO("resolve: " << record->resolveOutput << "; line: " << record->line);
        CHECK(record->adjudication.requested); // The grammar keeps the proposal valid.
        CHECK(record->adjudication.reasons.empty());
        CHECK_FALSE(record->line.empty());
        CHECK_FALSE(streamed.empty());
    }
    CHECK(session.Turns(id).size() == playerTexts.size());
}

TEST_CASE("The three-actor fixture on the real model: nobody remembers what it did not witness",
          "[llama][hosted][memory]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // As the test above: budgets that fit the shared backend's context of 4096, greedy sampling.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.0, 40, 0.95};
    settings.narrateSampling = {0.0, 40, 0.95};
    // The model writes the lines and the memories: only what cannot depend on its words is checked.
    test::PlayMemoryFixture("three_actors.json", backend, settings, false);
}

TEST_CASE("The reload fixture on the real model: a rewound run forgets what it hid", "[llama][hosted][rewind]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // As the test above: budgets that fit the shared backend's context of 4096, greedy sampling.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.0, 40, 0.95};
    settings.narrateSampling = {0.0, 40, 0.95};
    // The rewind counts, turns, conversations and memory counts never depend on the model's words.
    test::PlayMemoryFixture("reload.json", backend, settings, false);
}

TEST_CASE("The prompt-pack fixture on the real model: the host's texts replace the defaults", "[llama][hosted][prompts]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // As the tests above: budgets that fit the shared backend's context of 4096, greedy sampling.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.0, 40, 0.95};
    settings.narrateSampling = {0.0, 40, 0.95};
    // The prompts' texts never depend on the model's words: every prompt_absent is checked.
    test::PlayMemoryFixture("prompt_pack.json", backend, settings, false);
}
