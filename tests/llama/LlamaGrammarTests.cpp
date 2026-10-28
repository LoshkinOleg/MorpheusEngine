#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <string>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;

TEST_CASE("llama.cpp accepts the generated grammars and they constrain real output", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
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

    // A grammar llama.cpp cannot parse throws ContractError, so short runs prove each one compiles.
    inference::GenerationSpec probe;
    probe.maxTokens = 4;
    probe.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Assisted);
    CHECK_NOTHROW(backend.Complete(prompt, probe, {}, {}));
    probe.grammar = turn::BuildLookupGrammar();
    CHECK_NOTHROW(backend.Complete(prompt, probe, {}, {}));

    inference::GenerationSpec spec;
    spec.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);
    spec.maxTokens = 512;
    const inference::Completion completion = backend.Complete(prompt, spec, {}, {});
    // Grammar-constrained decode speed on this machine (recorded in the task report).
    WARN(std::format("resolve: {} tokens in {:.0f} ms ({:.1f} tok/s); output: {}", completion.tokensGenerated,
                     completion.generateMs, completion.tokensGenerated * 1000.0 / std::max(1.0, completion.generateMs),
                     completion.text));
    REQUIRE_FALSE(completion.stoppedAtLimit);
    const nlohmann::json output = nlohmann::json::parse(completion.text);
    CHECK(output.at("intent").is_string());
    CHECK(output.at("checks").is_array());
    CHECK(output.at("changes").is_array());
}
