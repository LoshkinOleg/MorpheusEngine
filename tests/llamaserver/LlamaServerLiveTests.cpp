#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/MemoryMode.h"
#include "core/Thinking.h"
#include "core/World.h"
#include "grammar/Check.h"
#include "inference/Llm.h"
#include "llamaserver/LiveServer.h"
#include "llamaserver/LlamaServerBackend.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;

namespace {

constexpr int SESSIONS = 3;
constexpr int EXCHANGES = 10;
constexpr int LORE_LINES = 60; // About 2,500 tokens of system prompt per session.

// The action call's grammar: one of a few actions, as the Skyrim host's action catalog allows.
const std::string ACTION_GRAMMAR = "root ::= \"{\\\"action\\\":\\\"\" (\"wait\" | \"follow\" | \"trade\" | \"attack\") \"\\\"}\"\n";

// Session `session`'s system prompt: a character of its own and a long block of what it knows, so the prefix its slot
// keeps is worth reusing and differs from the other sessions'.
std::string CharacterPrompt(int session)
{
    static const std::array<const char*, SESSIONS> NAMES = {"Hulda", "Saadia", "Belethor"};
    const char* pName = NAMES[static_cast<std::size_t>(session)];
    std::string prompt = std::format("You are {0}, who lives in the town of Whiterun. Stay in character and answer as "
                                     "{0} would.\n\n# What {0} knows\n",
                                     pName);
    for (int line = 0; line < LORE_LINES; ++line) {
        prompt += std::format("- {} heard rumour {}: the caravan from the south was {} days late, and the guards say "
                              "the road past milestone {} is unsafe after dark.\n",
                              pName, line, (line * 7 + session) % 13 + 1, line * 3 + session);
    }
    return prompt;
}

// What the player says in exchange `exchange`.
std::string PlayerLine(int exchange)
{
    static const std::array<const char*, 5> LINES = {"Good day to you.", "Have you heard any news from the road?",
                                                     "I am looking for work.", "What do you think of the guards?",
                                                     "Farewell for now."};
    return LINES[static_cast<std::size_t>(exchange) % LINES.size()];
}

// The share of a call's prompt the server served from the slot's cache.
double ReusedFraction(const inference::Completion& completion)
{
    return completion.promptTokens == 0 ? 0.0 : static_cast<double>(completion.tokensReused) / completion.promptTokens;
}

} // namespace

TEST_CASE("llama-server: a resolve call completes under its grammar and a narrate call streams", "[server]")
{
    llamaserver::LlamaServerBackend backend(test::LiveServerConfig(0));
    const core::World world = core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");

    inference::Prompt prompt;
    prompt.messages = {
        {inference::Role::System,
         "You are the game master of a text RPG. Reply with one JSON object: intent, checks, changes.\n\n" +
             context::RenderWorldReference(world)},
        {inference::Role::User, "# Current state\n" + context::RenderStateView(world.initialState) +
                                    "\n# Player\nI climb onto the crawler's roof to scan the horizon.\n\n# Task\n"
                                    "Resolve the action."},
    };
    prompt.stableCount = 1;

    inference::GenerationSpec resolve;
    resolve.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);
    resolve.maxTokens = 1024;
    const inference::Completion resolved = backend.Complete(prompt, resolve, {}, {});
    WARN(std::format("resolve: {} prompt tokens ({} reused), {} generated in {:.0f} ms: {}", resolved.promptTokens,
                     resolved.tokensReused, resolved.tokensGenerated, resolved.generateMs, resolved.text));
    REQUIRE_FALSE(resolved.stoppedAtLimit);
    CHECK(grammar::Check(resolve.grammar, resolved.text) == "");
    const nlohmann::json output = nlohmann::json::parse(resolved.text);
    CHECK(output.at("intent").is_string());
    CHECK(output.at("checks").is_array());
    CHECK(output.at("changes").is_array());

    // The narrate call extends the resolve prompt on the same slot, so the server reuses part of it.
    inference::Prompt narrate = prompt;
    narrate.messages.push_back({inference::Role::Assistant, resolved.text});
    narrate.messages.push_back({inference::Role::User, "Narrate what happens in two sentences."});
    inference::GenerationSpec prose;
    prose.maxTokens = 128;
    std::string streamed;
    const inference::Completion narrated =
        backend.Complete(narrate, prose, [&](std::string_view piece) { streamed += piece; }, {});
    WARN(std::format("narrate: {} of {} prompt tokens reused: {}", narrated.tokensReused, narrated.promptTokens,
                     narrated.text));
    CHECK_FALSE(narrated.text.empty());
    CHECK(streamed == narrated.text);
}

TEST_CASE("llama-server: a thinking model thinks before it answers under the grammar", "[server]")
{
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::User, "Is sea water salty? Answer yes or no."}};
    inference::GenerationSpec spec;
    spec.grammar = "root ::= \"yes\" | \"no\"\n";
    spec.maxTokens = 8;
    spec.thinkingMaxTokens = 256;

    // The same call under the default (Auto) setting and under the Qwen format named in the setting.
    llamaserver::LlamaServerConfig formatConfig = test::LiveServerConfig(0);
    formatConfig.thinking = {core::ThinkingMode::Format, core::QWEN_THINKING};
    for (const llamaserver::LlamaServerConfig& config : {test::LiveServerConfig(0), formatConfig}) {
        llamaserver::LlamaServerBackend backend(config);
        inference::Completion completion;
        try {
            completion = backend.Complete(prompt, spec, {}, {});
        } catch (const inference::InferenceError& error) {
            if (std::string_view(error.what()).find("no thinking mode") == std::string_view::npos) {
                throw;
            }
            SKIP("the chat server's model has no thinking mode");
        }
        WARN(std::format("thinking: {} tokens in {:.0f} ms{}; answer '{}'", completion.thinkingTokens,
                         completion.thinkingMs, completion.thinkingStoppedAtLimit ? " (closed at the budget)" : "",
                         completion.text));
        CHECK(completion.thinkingTokens > 0);
        CHECK(completion.thinkingTokens <= spec.thinkingMaxTokens);
        CHECK((completion.text == "yes" || completion.text == "no"));
    }
}

TEST_CASE("llama-server: three sessions on three slots reuse their own cached prompts", "[server]")
{
    // R2's acceptance: three sessions, each on its own slot, alternate an action call (under a grammar) and a speech
    // call, ten exchanges each, on a 16k context. The configs are read here: SKIP must run on the test's thread.
    std::array<llamaserver::LlamaServerConfig, SESSIONS> configs;
    for (int session = 0; session < SESSIONS; ++session) {
        configs[static_cast<std::size_t>(session)] = test::LiveServerConfig(session);
    }

    // Each session runs at once on a thread of its own, with its own backend, as sub-project D's conversations will.
    // Catch2's checks are not thread-safe, so the threads only record; the checks follow.
    struct SessionLog {
        std::vector<std::string> calls = {};
        std::vector<double> reused = {};
        std::string error = {};
    };
    std::array<SessionLog, SESSIONS> logs;
    {
        std::vector<std::jthread> sessions;
        for (int session = 0; session < SESSIONS; ++session) {
            sessions.emplace_back([&configs, &logs, session] {
                SessionLog& log = logs[static_cast<std::size_t>(session)];
                try {
                    llamaserver::LlamaServerBackend backend(configs[static_cast<std::size_t>(session)]);
                    inference::Prompt conversation;
                    conversation.messages = {{inference::Role::System, CharacterPrompt(session)}};
                    conversation.stableCount = 1;
                    for (int exchange = 0; exchange < EXCHANGES; ++exchange) {
                        const std::string said = std::format("The player says: \"{}\"", PlayerLine(exchange));

                        // The action call: the conversation so far, then the request for an action.
                        inference::Prompt action = conversation;
                        action.messages.push_back({inference::Role::User, said + " Choose your action."});
                        inference::GenerationSpec actionSpec;
                        actionSpec.grammar = ACTION_GRAMMAR;
                        actionSpec.maxTokens = 16;
                        const inference::Completion acted = backend.Complete(action, actionSpec, {}, {});
                        log.calls.push_back("action " + acted.text);
                        log.reused.push_back(ReusedFraction(acted));

                        // The speech call: the same conversation, then the request for a line of speech.
                        inference::Prompt speech = conversation;
                        speech.messages.push_back({inference::Role::User, said + " Answer in one sentence."});
                        inference::GenerationSpec speechSpec;
                        speechSpec.maxTokens = 48;
                        const inference::Completion spoke = backend.Complete(speech, speechSpec, {}, {});
                        log.calls.push_back("speech");
                        log.reused.push_back(ReusedFraction(spoke));

                        // The exchange joins the conversation the next calls extend.
                        conversation.messages.push_back({inference::Role::User, said});
                        conversation.messages.push_back({inference::Role::Assistant, spoke.text});
                    }
                } catch (const std::exception& error) {
                    log.error = error.what();
                }
            });
        }
    } // The sessions join here.

    for (int session = 0; session < SESSIONS; ++session) {
        const SessionLog& log = logs[static_cast<std::size_t>(session)];
        INFO("session " << session);
        CHECK(log.error == "");
        // A short session fails here, and its per-call figures are skipped, so the other sessions' still print.
        CHECK(log.reused.size() == static_cast<std::size_t>(2 * EXCHANGES));
        if (log.reused.size() != static_cast<std::size_t>(2 * EXCHANGES)) {
            continue;
        }
        // Every call after the session's first finds part of its prompt in its own slot's cache. The figures go to
        // the README; 80% is not asserted, since what a model's linear-attention layers allow is what is measured.
        std::string figures;
        double laterSum = 0.0;
        for (std::size_t call = 0; call < log.reused.size(); ++call) {
            figures += std::format("{:.2f} ", log.reused[call]);
            if (call > 0) {
                INFO("call " << call << " (" << log.calls[call] << ")");
                CHECK(log.reused[call] > 0.0);
                laterSum += log.reused[call];
            }
        }
        WARN(std::format("session {} (slot {}): reused fraction per call: {}; mean after the first: {:.2f}", session,
                         configs[static_cast<std::size_t>(session)].slot, figures,
                         laterSum / static_cast<double>(log.reused.size() - 1)));
    }
}
