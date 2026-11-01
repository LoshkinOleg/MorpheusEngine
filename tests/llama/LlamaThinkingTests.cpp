#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/Errors.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// A sandcrawler resolve prompt: worth some thought, answered with the resolve grammar.
inference::Prompt ResolvePrompt(const core::World& world)
{
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
    return prompt;
}

inference::Prompt QuestionPrompt(std::string question)
{
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::User, std::move(question)}};
    return prompt;
}

} // namespace

TEST_CASE("A model without a thinking mode refuses a thinking budget", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    inference::GenerationSpec spec;
    spec.maxTokens = 8;
    spec.thinkingMaxTokens = 128;
    CHECK_THROWS_WITH(backend.Complete(QuestionPrompt("Say hi."), spec, {}, {}), ContainsSubstring("no thinking mode"));
}

TEST_CASE("A thinking model thinks, then writes a grammar-constrained answer", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedThinkingBackend();
    const core::World world = core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");
    const inference::Prompt prompt = ResolvePrompt(world);

    inference::GenerationSpec spec;
    spec.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);
    spec.maxTokens = 1024;
    spec.thinkingMaxTokens = 2048;
    std::string streamed;
    const inference::Completion completion =
        backend.Complete(prompt, spec, [&](std::string_view piece) { streamed += piece; }, {});
    WARN(std::format("thinking: {} tokens in {:.0f} ms (stopped at limit: {}); answer: {} tokens in {:.0f} ms",
                     completion.thinkingTokens, completion.thinkingMs, completion.thinkingStoppedAtLimit,
                     completion.tokensGenerated, completion.generateMs));

    // The block stays out of the answer and the stream; the answer alone follows the grammar.
    CHECK_FALSE(completion.thinking.empty());
    CHECK(completion.thinkingTokens <= spec.thinkingMaxTokens);
    CHECK(streamed == completion.text);
    CHECK_THAT(completion.text, !ContainsSubstring("<think>") && !ContainsSubstring("</think>"));
    REQUIRE_FALSE(completion.stoppedAtLimit);
    const nlohmann::json output = nlohmann::json::parse(completion.text);
    CHECK(output.at("intent").is_string());

    // The thinking call evaluates no more prompt than CountPromptTokens promises.
    CHECK(completion.promptTokens <= backend.CountPromptTokens(prompt));
}

TEST_CASE("A thinking block that runs out of budget is closed by the backend", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedThinkingBackend();
    inference::GenerationSpec spec;
    spec.grammar = R"(root ::= [0-9]+)";
    spec.maxTokens = 16;
    spec.thinkingMaxTokens = 64; // Far too little for the question below.
    const inference::Completion completion = backend.Complete(
        QuestionPrompt("How many prime numbers are there below 200? Check each candidate carefully."), spec, {}, {});

    // The backend closed the block with its own text, which exactly fills the budget, and an answer still came.
    CHECK(completion.thinkingStoppedAtLimit);
    CHECK(completion.thinkingTokens == spec.thinkingMaxTokens);
    CHECK_FALSE(completion.text.empty());
}

TEST_CASE("Without a thinking budget a thinking model answers straight away", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedThinkingBackend();
    const inference::Prompt prompt = QuestionPrompt("Name one colour of the rainbow. One word.");
    inference::GenerationSpec spec;
    spec.maxTokens = 16;
    const inference::Completion completion = backend.Complete(prompt, spec, {}, {});

    // The prompt ends with the empty block, so the reply is the answer alone.
    CHECK(completion.promptTokens == backend.CountPromptTokens(prompt));
    CHECK(completion.thinking.empty());
    CHECK(completion.thinkingTokens == 0);
    CHECK_FALSE(completion.text.empty());
    CHECK_THAT(completion.text, !ContainsSubstring("<think>") && !ContainsSubstring("</think>"));
}

TEST_CASE("A thinking budget too small to close the block is a contract error", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedThinkingBackend();
    inference::GenerationSpec spec;
    spec.maxTokens = 8;
    spec.thinkingMaxTokens = 2;
    CHECK_THROWS_AS(backend.Complete(QuestionPrompt("Say hi."), spec, {}, {}), core::ContractError);
}
