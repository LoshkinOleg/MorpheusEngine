#include "context/PromptBuilder.h"

#include <algorithm>
#include <format>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "core/Errors.h"

namespace morpheus::context {

Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.engineRules.empty() && !inputs.playerInput.empty() && !inputs.instruction.empty(),
                     "AssembleResolvePrompt: engine rules, player input and instruction are required");

    // L1: the world's rules, then its always-on lore.
    std::string worldText = "# World\n" + world.rules;
    if (!worldText.ends_with('\n')) {
        worldText += '\n';
    }
    std::string lore;
    for (const core::LoreEntry& entry : world.lore) {
        if (entry.alwaysOn) {
            lore += std::format("- {}: {}\n", entry.subject, entry.data);
        }
    }
    if (!lore.empty()) {
        worldText += "# Lore\n" + lore;
    }

    // L2: folded summaries, oldest first.
    std::string summaryText;
    if (!inputs.summaries.empty()) {
        summaryText = "# Story so far\n";
        for (const core::Summary& summary : inputs.summaries) {
            summaryText += std::format("Turns {}-{}: {}\n", summary.fromTurn, summary.toTurn, summary.text);
        }
    }

    // T1-T4: rebuilt every call, so it goes last.
    std::string tail = "# Current state\n" + RenderStateView(inputs.state);
    if (!inputs.retrieved.empty()) {
        tail += "\n# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
    }
    tail += "\n# Player\n" + inputs.playerInput + "\n\n# Task\n" + inputs.instruction;

    // Messages: stable prefix (system, transcript), then the tail.
    Assembly assembly;
    inference::Prompt& prompt = assembly.prompt;
    prompt.messages.push_back({inference::Role::System, inputs.engineRules + "\n\n" + worldText + summaryText});
    for (inference::Message& message : RenderTranscript(inputs.transcript)) {
        prompt.messages.push_back(std::move(message));
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, tail});

    // Exact counts with the model's tokenizer.
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.engineRules);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    for (std::size_t i = 1; i < prompt.stableCount; ++i) {
        tokens.transcript += backend.CountTokens(prompt.messages[i].content);
    }
    tokens.tail = backend.CountTokens(tail);
    tokens.total = backend.CountPromptTokens(prompt);

    // Fixed layers: L0 is engine text (a bug if too long), L1 is world data (the author must trim it).
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("engine rules use {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("world '{}': rules and always-on lore use {} tokens, over world_cap {}",
                                          world.id, tokens.world, budget.worldCap));
    }
    assembly.summaryOverBudget = tokens.summary > budget.summaryCap;
    assembly.tailOverBudget = tokens.tail > budget.tailAllowance;

    // L3 gets what the fixed layers and the reserved generations leave.
    assembly.transcriptBudget = backend.ContextSize() - budget.resolveMaxTokens - budget.narrateMaxTokens -
                                budget.tailAllowance - tokens.engineRules - tokens.world -
                                std::min(tokens.summary, budget.summaryCap);
    assembly.transcriptOverBudget = tokens.transcript > assembly.transcriptBudget;

    // Template tokens are not in the layer counts; this catches them overflowing the context.
    assembly.contextOverflow =
        tokens.total + budget.resolveMaxTokens + budget.narrateMaxTokens > backend.ContextSize();
    return assembly;
}

void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction)
{
    MORPHEUS_REQUIRE(!prompt.messages.empty() && prompt.messages.back().role == inference::Role::User,
                     "ExtendForNarrate: expects a resolve prompt ending with the user tail");
    prompt.messages.push_back({inference::Role::Assistant, resolveOutput});
    prompt.messages.push_back({inference::Role::User, resolutionAndInstruction});
}

} // namespace morpheus::context
