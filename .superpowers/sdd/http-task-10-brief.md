### Task 10: The [server] suite: a real llama-server, and R2's three-slot reuse

Tests against a `llama-server` the owner starts, skipped unless `MORPHEUS_TEST_SERVER_URL` and `MORPHEUS_TEST_EMBED_SERVER_URL` are set. **No llama-server runs on this machine: every test of this task is skipped here.** They are written to compile and skip cleanly; the owner runs them (README, Task 11) and records the reuse figures.

**Files:**
- Create: `tests/TestEnvironment.h`
- Modify: `tests/llama/LlamaTestModels.h`
- Create: `tests/llamaserver/LiveServer.h`
- Test: `tests/llamaserver/LlamaServerLiveTests.cpp` (`[server]`, every preset with the llama-server backend)
- Test: `tests/llama/LlamaServerAgreementTests.cpp` (`[server][llama]`, dev preset only)

**Interfaces:**
- Consumes: `llamaserver::LlamaServerBackend` (Tasks 5-6), `grammar::Check` (Task 2), `test::SharedBackend()` (`tests/llama/LlamaTestModels.h`).
- Produces: `std::string morpheus::test::EnvironmentVariable(const char* name)` in `tests/TestEnvironment.h` (moved from `LlamaTestModels.h`, unchanged); `constexpr int test::LIVE_CONTEXT_SIZE = 16384` and `llamaserver::LlamaServerConfig test::LiveServerConfig(int slot)` in `tests/llamaserver/LiveServer.h`.

- [ ] **Step 1: Move EnvironmentVariable out of the llama-only header**

Create `tests/TestEnvironment.h`:

```cpp
#pragma once

#include <cstdlib>
#include <string>

namespace morpheus::test {

// Reads an environment variable; "" when unset. _dupenv_s avoids MSVC's getenv deprecation warning under /WX.
inline std::string EnvironmentVariable(const char* name)
{
#ifdef _MSC_VER
    char* pValue = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&pValue, &length, name) != 0 || pValue == nullptr) {
        return {};
    }
    std::string value(pValue);
    std::free(pValue);
    return value;
#else
    const char* pValue = std::getenv(name);
    return pValue != nullptr ? pValue : "";
#endif
}

} // namespace morpheus::test
```

In `tests/llama/LlamaTestModels.h`, delete the `EnvironmentVariable` function with its comment, delete `#include <cstdlib>`, and add `#include "TestEnvironment.h"` before `#include "llama/LlamaCppBackend.h"`.

- [ ] **Step 2: Write the [server] tests**

Create `tests/llamaserver/LiveServer.h`:

```cpp
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "TestEnvironment.h"
#include "llamaserver/LlamaServerBackend.h"

namespace morpheus::test {

// The [server] tests run against a llama-server the owner starts (README, "The [server] tests"):
//   MORPHEUS_TEST_SERVER_URL        the chat server, e.g. http://127.0.0.1:8080, started with --parallel 3 or more and
//                                   at least LIVE_CONTEXT_SIZE tokens per slot
//   MORPHEUS_TEST_EMBED_SERVER_URL  the embedding server (--embedding), e.g. http://127.0.0.1:8081
// The calling test is skipped when either is unset.
constexpr int LIVE_CONTEXT_SIZE = 16384;

// A config for `slot` of the owner's servers, with nomic-embed's prefixes. Skips the calling test when a variable is
// unset; call it on the test's own thread.
inline llamaserver::LlamaServerConfig LiveServerConfig(int slot)
{
    const std::string chatUrl = EnvironmentVariable("MORPHEUS_TEST_SERVER_URL");
    const std::string embeddingUrl = EnvironmentVariable("MORPHEUS_TEST_EMBED_SERVER_URL");
    if (chatUrl.empty() || embeddingUrl.empty()) {
        SKIP("set MORPHEUS_TEST_SERVER_URL and MORPHEUS_TEST_EMBED_SERVER_URL to run the [server] tests");
    }
    llamaserver::LlamaServerConfig config;
    config.chatUrl = chatUrl;
    config.embeddingUrl = embeddingUrl;
    config.slot = slot;
    config.contextSize = LIVE_CONTEXT_SIZE;
    config.embeddingQueryPrefix = "search_query: ";
    config.embeddingDocumentPrefix = "search_document: ";
    return config;
}

} // namespace morpheus::test
```

Create `tests/llamaserver/LlamaServerLiveTests.cpp`:

```cpp
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
    llamaserver::LlamaServerBackend backend(test::LiveServerConfig(0));
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::User, "Is sea water salty? Answer yes or no."}};
    inference::GenerationSpec spec;
    spec.grammar = "root ::= \"yes\" | \"no\"\n";
    spec.maxTokens = 8;
    spec.thinkingMaxTokens = 256;

    inference::Completion completion;
    try {
        completion = backend.Complete(prompt, spec, {}, {});
    } catch (const inference::InferenceError& error) {
        if (std::string_view(error.what()).find("no thinking mode") == std::string_view::npos) {
            throw;
        }
        SKIP("the chat server's model has no thinking mode");
    }
    WARN(std::format("thinking: {} tokens in {:.0f} ms{}; answer '{}'", completion.thinkingTokens, completion.thinkingMs,
                     completion.thinkingStoppedAtLimit ? " (closed at the budget)" : "", completion.text));
    CHECK(completion.thinkingTokens > 0);
    CHECK(completion.thinkingTokens <= spec.thinkingMaxTokens);
    CHECK((completion.text == "yes" || completion.text == "no"));
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
        REQUIRE(log.reused.size() == static_cast<std::size_t>(2 * EXCHANGES));
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
```

Create `tests/llama/LlamaServerAgreementTests.cpp`:

```cpp
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
```

- [ ] **Step 3: Run the tests (they skip here)**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[server]"`
Expected: builds with no warnings; the three test cases are reported as skipped ("set MORPHEUS_TEST_SERVER_URL and MORPHEUS_TEST_EMBED_SERVER_URL ..."), none failed.

Run: `ctest --preset tests`
Expected: `0 tests failed out of 412`, with the three `llama-server: ...` tests listed as "Not Run (Skipped)".

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests && ./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[server]"`
Expected: builds with no warnings; four test cases skipped (the agreement test skips at `LiveServerConfig`, or at `SharedBackend` when the model variables are unset).

- [ ] **Step 4: Byte check and commit**

```bash
check_bytes tests/TestEnvironment.h tests/llama/LlamaTestModels.h tests/llamaserver/LiveServer.h tests/llamaserver/LlamaServerLiveTests.cpp tests/llama/LlamaServerAgreementTests.cpp
git add tests/TestEnvironment.h tests/llama/LlamaTestModels.h tests/llamaserver/LiveServer.h tests/llamaserver/LlamaServerLiveTests.cpp tests/llama/LlamaServerAgreementTests.cpp
git commit -m "Add the [server] tests: a real llama-server, token agreement, and R2's three-slot reuse" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Expected: `check_bytes` prints nothing (`LlamaServerAgreementTests.cpp`'s non-ASCII text must stay `\x` escapes).

---

