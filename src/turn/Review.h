#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
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

// The review call of a hosted turn (hosted-review spec, section 2), standalone: `rules` (the run's pack `review`) as the
// system message, then one user message: "# Speaker\n<name>\n\n", "# Player\n<playerLine>\n\n" when there is one,
// "# Requested action\n<id>" (then " <params as compact JSON>" when it has parameters; "none (the proposal was
// refused)" for a refused one), "\n\n# Observation\n<compact JSON>\n\n# Line\n<line>\n\n# Task\n" and
// HOSTED_REVIEW_INSTRUCTION. stableCount is 1 (the rules).
inference::Prompt HostedReviewPrompt(std::string_view rules, std::string_view speakerName, std::string_view playerLine,
                                     const core::Adjudication& adjudication, const nlohmann::json& observation,
                                     std::string_view line);

} // namespace morpheus::turn
