#include "context/PromptBuilder.h"

#include <format>
#include <utility>

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

    // The fixed part of the prompt: the system message and the tail. Counting it, with and without the tail,
    // gives the template-inclusive costs the transcript budget needs.
    Assembly assembly;
    inference::Prompt fixed;
    fixed.messages.push_back({inference::Role::System, inputs.engineRules + "\n\n" + worldText + summaryText});
    fixed.messages.push_back({inference::Role::User, std::move(tail)});

    // Exact counts with the model's tokenizer. Layer counts are raw text; only `total` includes template tokens.
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.engineRules);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    tokens.tail = backend.CountTokens(fixed.messages.back().content);
    const int fixedTotal = backend.CountPromptTokens(fixed); // System message and tail, no transcript.
    inference::Message tailMessage = std::move(fixed.messages.back());
    fixed.messages.pop_back();
    const int systemCost = backend.CountPromptTokens(fixed); // The system message alone, templated.

    // Messages: stable prefix (system, transcript), then the tail.
    inference::Prompt& prompt = assembly.prompt;
    prompt.messages.push_back(std::move(fixed.messages.front()));
    for (inference::Message& message : RenderTranscript(inputs.transcript)) {
        tokens.transcript += backend.CountTokens(message.content);
        prompt.messages.push_back(std::move(message));
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back(std::move(tailMessage));
    tokens.total = backend.CountPromptTokens(prompt);
    assembly.transcriptCost = tokens.total - fixedTotal;

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

    // L3 gets what the templated system message, the tail allowance, the narrate allowance and both generations
    // leave.
    assembly.transcriptBudget = backend.ContextSize() - budget.resolveMaxTokens - budget.narrateMaxTokens -
                                budget.narrateAllowance - budget.tailAllowance - systemCost;
    if (assembly.transcriptBudget <= 0) {
        // Folding turns cannot fix this: the settings or the world leave L3 nothing, so it is not a flag.
        throw core::DataError(std::format("context_size {} leaves no room for the transcript after the system "
                                          "layers ({} tokens), the tail allowance, the narrate allowance and both "
                                          "generations",
                                          backend.ContextSize(), systemCost));
    }
    assembly.transcriptOverBudget = assembly.transcriptCost > assembly.transcriptBudget;

    // Final safety check on the whole templated prompt, as seen by the narrate call: the prompt, the narrate
    // message (its template overhead plus the allowance for its text), the resolve output's assistant message
    // (template overhead only: its text is the resolve generation) and both generations. Each overhead is
    // approximated by the cost of an empty one-message prompt.
    inference::Prompt emptyUser;
    emptyUser.messages.push_back({inference::Role::User, ""});
    const int narrateMessageOverhead = backend.CountPromptTokens(emptyUser);
    inference::Prompt emptyAssistant;
    emptyAssistant.messages.push_back({inference::Role::Assistant, ""});
    const int resolveOutputOverhead = backend.CountPromptTokens(emptyAssistant);
    assembly.contextOverflow = tokens.total + budget.narrateAllowance + narrateMessageOverhead +
                                   resolveOutputOverhead + budget.resolveMaxTokens + budget.narrateMaxTokens >
                               backend.ContextSize();
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
