#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <string>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/Changes.h"
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
    spec.maxTokens = 1024; // Room for a valid but long proposal.
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

    // The output is meaningful to the engine: stats and difficulties are the world's, and every change entry has
    // the shape ParseChange reads.
    nlohmann::json allChanges = output.at("changes");
    for (const nlohmann::json& check : output.at("checks")) {
        CHECK(std::ranges::find(world.stats, check.at("stat").get<std::string>()) != world.stats.end());
        CHECK(world.difficulties.contains(check.at("difficulty").get<std::string>()));
        for (const char* branch : {"on_success", "on_failure"}) {
            for (const nlohmann::json& change : check.at(branch)) {
                allChanges.push_back(change);
            }
        }
    }
    for (const nlohmann::json& raw : allChanges) {
        // A change the grammar let through but ParseChange cannot read would be a grammar bug.
        std::string error;
        INFO("change: " << raw.dump());
        REQUIRE(core::ParseChange(raw, error).has_value());
    }
    // Value problems (out-of-range ints, unknown entity ids for creates) are legitimate engine rejections of what
    // the model proposed: shown, not failed.
    const core::ApplyResult applied = core::ApplyChanges(world.schema, world.initialState, allChanges);
    for (const core::RejectedChange& rejected : applied.rejected) {
        WARN(std::format("rejected on apply: {} -- {}", rejected.raw.dump(), rejected.reason));
    }
}
