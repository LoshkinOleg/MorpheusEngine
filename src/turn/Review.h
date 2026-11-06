#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"
#include "turn/Resolve.h"

namespace morpheus::turn {

// The review call's output, read (architecture spec, "Turn flow"). `consistent` and `problems` come from the
// narration check, `digest` from the compact transcript. Output that cannot be read leaves everything empty except
// `error`, which says why: a model mistake, so the turn goes on as if the review had found nothing.
struct Review {
    std::optional<bool> consistent = {};    // Empty when the check was off or the output could not be read.
    std::vector<std::string> problems = {}; // The contradictions; empty when consistent.
    std::string digest = {};                // Trimmed. Empty when the compact transcript was off.
    std::string error = {};
};

// Reads the output of a review call made with BuildReviewGrammar(check, digest): with `check`, a boolean
// "consistent" and an array of string "problems" that is empty exactly when "consistent" is true; with `digest`, a
// string "digest". Fields that were not asked for are ignored. Throws ContractError unless `check` or `digest` is set.
Review ParseReviewOutput(std::string_view text, bool check, bool digest);

// The review call's prompt, standalone: REVIEW_RULES as the system message, then one user message:
//   # Player
//   <input>
//
//   <RenderOutcome(proposal, resolution, {})>
//   # Narration
//   <narration>
//
//   # Task
//   <REVIEW_INSTRUCTION>
// stableCount is 1 (the rules).
inference::Prompt ReviewPrompt(const std::string& input, const ResolveProposal& proposal,
                               const Resolution& resolution, const std::string& narration);

} // namespace morpheus::turn
