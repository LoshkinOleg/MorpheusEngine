#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/Transcript.h"
#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"

namespace morpheus::context {

// What goes into one resolve prompt, by layer (architecture spec, "Context layout and KV cache"). The world
// (L1) is passed separately.
struct ContextInputs {
    std::string engineRules = {};                  // L0: engine rules and the output format.
    std::vector<core::Summary> summaries = {};     // L2: folded turns, oldest first.
    std::vector<TranscriptEntry> transcript = {};  // L3: unfolded turns, oldest first, rendered (TranscriptEntries).
    nlohmann::json state = nullptr;                // T1: current state (validated).
    std::vector<std::string> retrieved = {};       // T2: formatted lore/history snippets.
    std::string playerInput = {};                  // T3
    std::string instruction = {};                  // T4: the resolve task instruction.
};

// What goes into one hosted resolve prompt, by layer (hosted-world spec, "The prompt"), most stable first, so a new
// turn adds to the end of the cached part.
struct HostedInputs {
    std::string systemText = {};                      // L0: the hosted system text.
    std::vector<core::LoreEntry> lore = {};           // L1: the host's lore; only the always-on entries are shown.
    std::vector<core::Participant> participants = {}; // The participants, in join order: name, id and bio.
    std::vector<core::Summary> summaries = {};        // L2: run-wide summaries of folded turns, oldest first.
    std::vector<TranscriptEntry> transcript = {};     // L3: this conversation's unfolded turns, rendered.
    std::vector<std::string> retrieved = {};          // Tail: lore and history snippets.
    nlohmann::json observation = nullptr;             // Tail: shown as compact JSON.
    std::vector<std::string> events = {};             // Tail: the new events' texts, in order.
    std::string playerLine = {};                      // Tail: "<player name>: <text>", or empty.
    std::string instruction = {};                     // Tail: the resolve instruction.
};

// Exact token counts. Layer counts are raw text (no template tokens), for the inspector; `total` is the only
// figure that includes chat-template tokens (the whole templated prompt).
struct LayerTokens {
    int engineRules = 0; // L0.
    int world = 0;       // L1; in a hosted prompt, the host's always-on lore and the participants.
    int summary = 0;
    int transcript = 0;
    int tail = 0;
    int total = 0;
};

struct Assembly {
    inference::Prompt prompt = {};
    LayerTokens tokens = {};
    int transcriptBudget = 0;          // Tokens L3 may add to the templated prompt, with the summary counted at most at summary_cap (always positive).
    int transcriptCost = 0;            // Tokens L3 adds to the templated prompt, template tokens included.
    bool transcriptOverBudget = false; // Fold old turns into the summary and assemble again.
    bool summaryOverBudget = false;    // The summary itself must be condensed.
    bool tailOverBudget = false;       // The tail (state or observation, retrieval, input and instruction) exceeds tail_allowance.
    // The full templated prompt plus both generations exceeds the context: shrink the tail or fold.
    bool contextOverflow = false;

    bool Fits() const
    {
        return !transcriptOverBudget && !summaryOverBudget && !tailOverBudget && !contextOverflow;
    }
};

// Builds the resolve prompt: one system message (L0 + L1 + L2), the transcript (L3), then one user message with the
// volatile tail (T1-T4). L1 is the world's rules, game reference (RenderWorldReference) and always-on lore.
// prompt.stableCount marks the end of L3. Throws ContractError if L0 exceeds engine_rules_cap (engine text too
// long) and DataError if L1 exceeds world_cap (the world is too big) or if the context leaves no room for the
// transcript after the system message (its summary counted at most at summary_cap), the tail allowance, the narrate
// allowance and both generations, thinking blocks included (folding cannot fix that). Other overruns, an over-cap
// summary among them, are flags for the turn layer to act on.
//
// The narrate call appends two messages that are not part of this prompt: the resolve output (assistant, up to
// resolveMaxTokens) and the resolution record + instruction (user). budget.narrateAllowance is reserved for the
// user message's text: transcriptBudget excludes it. contextOverflow counts it together with the template
// overhead of both messages (assistant and user), each approximated by the cost of an empty one-message prompt.
Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend);

// Builds a hosted resolve prompt: one system message (L0, then L1 the always-on lore, then the participants as
// "- <name> (<id>): <bio>" or "- <name> (<id>)", then L2), the conversation's entries (L3), then one user message with
// the tail (retrieved snippets, the observation as compact JSON, the new events in parentheses, the player's line,
// the instruction). prompt.stableCount marks the end of L3. A join or a leave changes the prompt from the
// participants on; a new turn only adds entries before the tail. Budgets and flags are AssembleResolvePrompt's, with
// the speak call in the narrate call's place: tokens.world counts the lore and the participants together against
// world_cap (a DataError over it: the host's data); the system text over engine_rules_cap is a ContractError.
Assembly AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget,
                              inference::LlmBackend& backend);

// Turns a resolve prompt into the narrate prompt by appending the resolve output (assistant) and the resolution
// record with the narrate instruction (user). It only appends, so the whole resolve prompt stays cached.
// The appended user message must stay within budget.narrate_allowance; the turn layer checks this.
void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction);

} // namespace morpheus::context
