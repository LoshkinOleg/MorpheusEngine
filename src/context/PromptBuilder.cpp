#include "context/PromptBuilder.h"

#include <algorithm>
#include <format>
#include <utility>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "context/WorldReference.h"
#include "core/Errors.h"

namespace morpheus::context {

namespace {

// L1's always-on lore as "- <subject>: <data>" lines, in order; empty when no entry is always-on. Both builders show
// the lore the same way.
std::string RenderAlwaysOnLore(const std::vector<core::LoreEntry>& lore)
{
    std::string text;
    for (const core::LoreEntry& entry : lore) {
        if (entry.alwaysOn) {
            text += std::format("- {}: {}\n", entry.subject, entry.data);
        }
    }
    return text;
}

// L2: the folded summaries under "# Story so far", oldest first; empty when there are none. The condense call reads
// this section, so both kinds of run render it the same way.
std::string RenderSummaries(const std::vector<core::Summary>& summaries)
{
    if (summaries.empty()) {
        return {};
    }
    std::string text = "# Story so far\n";
    for (const core::Summary& summary : summaries) {
        text += std::format("Turns {}-{}: {}\n", summary.fromTurn, summary.toTurn, summary.text);
    }
    return text;
}

// Lays out a prompt into `assembly`: one system message with `systemText`, the messages of the transcript entries
// (the stable prefix ends after them), then one user message with `tail`. Then measures it against `budget`: the
// layer counts the caller has not set (tail, transcript, total), the transcript's budget and cost, and every flag. The
// caller has set tokens.summary: an over-cap summary is counted at most at its cap. Throws DataError when the context
// leaves no room for the transcript (folding cannot fix that).
void LayOut(Assembly& assembly, std::string systemText, const std::vector<TranscriptEntry>& transcript,
            std::string tail, const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    // The caller passes a fresh result: this appends to its prompt and adds to its transcript count.
    MORPHEUS_REQUIRE(assembly.prompt.messages.empty() && assembly.tokens.transcript == 0,
                     "LayOut: expects a fresh assembly with no messages and no transcript tokens");

    // The fixed part of the prompt: the system message and the tail. Counting it gives the template-inclusive costs
    // the transcript budget needs. No prompt counted here lacks a user message: some chat templates (Qwen3.5's) refuse
    // a message list without one.
    inference::Prompt fixed;
    fixed.messages.push_back({inference::Role::System, std::move(systemText)});
    fixed.messages.push_back({inference::Role::User, std::move(tail)});

    // Exact counts with the model's tokenizer. Layer counts are raw text; only `total` includes template tokens.
    LayerTokens& tokens = assembly.tokens;
    tokens.tail = backend.CountTokens(fixed.messages.back().content);
    const int fixedTotal = backend.CountPromptTokens(fixed); // System message and tail, no transcript.
    // The templated system message, with the tail message's frame and the generation prompt: the fixed part less the
    // tail's text. Exact, because both backends tokenize the template pieces and each content separately, so the
    // tail's content adds exactly CountTokens(tail) to the count, wherever it sits. The tail allowance budgets only
    // that text, so its frame belongs here.
    const int systemCost = fixedTotal - tokens.tail;
    inference::Message tailMessage = std::move(fixed.messages.back());
    fixed.messages.pop_back();

    // Messages: stable prefix (system, transcript), then the tail.
    inference::Prompt& prompt = assembly.prompt;
    prompt.messages.push_back(std::move(fixed.messages.front()));
    for (const TranscriptEntry& entry : transcript) {
        for (const inference::Message& message : entry.messages) {
            tokens.transcript += backend.CountTokens(message.content);
            prompt.messages.push_back(message);
        }
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back(std::move(tailMessage));
    tokens.total = backend.CountPromptTokens(prompt);
    assembly.transcriptCost = tokens.total - fixedTotal;

    assembly.summaryOverBudget = tokens.summary > budget.summaryCap;
    assembly.tailOverBudget = tokens.tail > budget.tailAllowance;

    // L3 gets what the templated system message (with the tail's frame), the tail allowance, the narrate allowance and both generations
    // leave, with the summary counted at most at its cap: an over-cap summary is a flag (condense it), not a
    // settings error. A generation is a call's thinking block plus its answer. Reserving both thinking blocks is
    // conservative: the resolve call's block is not part of the narrate prompt, so the two never share a context.
    const int summaryExcess = std::max(0, tokens.summary - budget.summaryCap);
    assembly.transcriptBudget = backend.ContextSize() - budget.ResolveGenerationTokens() -
                                budget.NarrateGenerationTokens() - budget.narrateAllowance - budget.tailAllowance -
                                (systemCost - summaryExcess);
    if (assembly.transcriptBudget <= 0) {
        // Folding turns cannot fix this: the settings or the world leave L3 nothing, so it is not a flag.
        throw core::DataError(std::format("context_size {} leaves no room for the transcript after the system "
                                          "layers ({} tokens), the tail allowance, the narrate allowance and both "
                                          "generations",
                                          backend.ContextSize(), systemCost - summaryExcess));
    }
    assembly.transcriptOverBudget = assembly.transcriptCost > assembly.transcriptBudget;

    // Final safety check on the whole templated prompt, as seen by the narrate call: the prompt, the narrate
    // message (its template overhead plus the allowance for its text), the resolve output's assistant message
    // (template overhead only: its text is the resolve generation) and both generations. The narrate message's
    // overhead is approximated by the cost of an empty user prompt, and the assistant message's by what an empty one
    // adds after an empty user message (an assistant-only prompt is refused by some templates).
    inference::Prompt emptyUser;
    emptyUser.messages.push_back({inference::Role::User, ""});
    const int narrateMessageOverhead = backend.CountPromptTokens(emptyUser);
    inference::Prompt emptyExchange = emptyUser;
    emptyExchange.messages.push_back({inference::Role::Assistant, ""});
    const int resolveOutputOverhead = backend.CountPromptTokens(emptyExchange) - narrateMessageOverhead;
    assembly.contextOverflow = tokens.total + budget.narrateAllowance + narrateMessageOverhead +
                                   resolveOutputOverhead + budget.ResolveGenerationTokens() +
                                   budget.NarrateGenerationTokens() >
                               backend.ContextSize();
}

} // namespace

Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.engineRules.empty() && !inputs.playerInput.empty() && !inputs.instruction.empty(),
                     "AssembleResolvePrompt: engine rules, player input and instruction are required");

    // L1: the world's rules, its game reference, then its always-on lore.
    std::string worldText = "# World\n" + world.rules;
    if (!worldText.ends_with('\n')) {
        worldText += '\n';
    }
    worldText += "\n" + RenderWorldReference(world);
    const std::string lore = RenderAlwaysOnLore(world.lore);
    if (!lore.empty()) {
        worldText += "# Lore\n" + lore;
    }

    // L2: folded summaries, oldest first.
    const std::string summaryText = RenderSummaries(inputs.summaries);

    // T1-T4: rebuilt every call, so it goes last.
    std::string tail = "# Current state\n" + RenderStateView(inputs.state);
    if (!inputs.retrieved.empty()) {
        tail += "\n# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
    }
    tail += "\n# Player\n" + inputs.playerInput + "\n\n# Task\n" + inputs.instruction;

    // The system layers' counts, then their caps: L0 is engine text (a bug if too long), L1 is world data (the
    // author must trim it).
    Assembly assembly;
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.engineRules);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("engine rules use {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("world '{}': rules and always-on lore use {} tokens, over world_cap {}",
                                          world.id, tokens.world, budget.worldCap));
    }
    LayOut(assembly, inputs.engineRules + "\n\n" + worldText + summaryText, inputs.transcript, std::move(tail), budget,
           backend);
    return assembly;
}

Assembly AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget,
                              inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.systemText.empty() && !inputs.participants.empty() && !inputs.instruction.empty(),
                     "AssembleHostedPrompt: the system text, a participant and the instruction are required");

    // L1, then the participants: the host's always-on lore, then who takes part, in join order. A join or a leave
    // changes the prompt from the participants on; the lore before them stays cached.
    const std::string lore = RenderAlwaysOnLore(inputs.lore);
    std::string worldText = lore.empty() ? std::string() : "# Lore\n" + lore + "\n";
    worldText += "# Participants\n";
    for (const core::Participant& participant : inputs.participants) {
        worldText += participant.bio.empty()
                         ? std::format("- {} ({})\n", participant.name, participant.id)
                         : std::format("- {} ({}): {}\n", participant.name, participant.id, participant.bio);
    }

    // L2: run-wide summaries of folded turns, oldest first.
    const std::string summaryText = RenderSummaries(inputs.summaries);

    // The tail, rebuilt every turn, so it goes last: retrieved snippets, the observation, the new events, the
    // player's line and the instruction. The replace handler keeps a string that is not UTF-8 from throwing: the
    // observation is the host's data.
    std::string tail;
    if (!inputs.retrieved.empty()) {
        tail += "# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
        tail += "\n";
    }
    tail += "# Observation\n" + inputs.observation.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    if (!inputs.events.empty()) {
        tail += "\n# New events\n";
        for (const std::string& event : inputs.events) {
            tail += "(" + event + ")\n";
        }
    }
    if (!inputs.playerLine.empty()) {
        tail += "\n# Player\n" + inputs.playerLine + "\n";
    }
    tail += "\n# Task\n" + inputs.instruction;

    // The system layers' counts, then their caps: L0 is engine text (a bug if too long), the lore and the
    // participants are the host's data.
    Assembly assembly;
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.systemText);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("the hosted system text uses {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("host lore and participants use {} tokens, over world_cap {}", tokens.world,
                                          budget.worldCap));
    }
    const std::string separator = summaryText.empty() ? std::string() : "\n";
    LayOut(assembly, inputs.systemText + "\n\n" + worldText + separator + summaryText, inputs.transcript,
           std::move(tail), budget, backend);
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
