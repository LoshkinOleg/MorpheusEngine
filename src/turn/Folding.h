#pragma once

#include <optional>
#include <stop_token>
#include <vector>

#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Rough bytes per token; the fallback only needs to stay near the summary's token cap.
constexpr int FALLBACK_BYTES_PER_TOKEN = 4;

// The fold call's prompt: `systemMessage` (the resolve prompt's system message, L0-L2), `turns` as transcript
// messages, then SUMMARIZE_INSTRUCTION. stableCount ends after the turns. The session measures it before a fold.
inference::Prompt SummarizePrompt(const inference::Message& systemMessage, const std::vector<core::TurnRecord>& turns);

// The engine's summary of `turns`, written without a model: one "Turn N: <input>" line per turn, each followed by
// the turn's mechanics line when it has one, cut to maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers the turns'
// range. Throws ContractError for no turns.
core::Summary SummarizeFallback(const std::vector<core::TurnRecord>& turns, int maxTokens);

// Summarizes `turns` (the oldest unfolded turns, in order) with one call on SummarizePrompt. That prompt starts like
// the resolve prompt, so the KV cache serves most of it. The summary covers the turns' range; its text is trimmed.
// When the model writes nothing, the summary is SummarizeFallback(turns, spec.maxTokens).
// Returns nullopt when `stop` cancels the call. Throws ContractError for no turns or a non-system message, and
// InferenceError from the backend (among others when the prompt does not fit the context: the caller measures it).
std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<core::TurnRecord>& turns,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop);

// The condense call's prompt: `systemMessage` (whose L2 holds the summaries), then CONDENSE_INSTRUCTION.
inference::Prompt CondensePrompt(const inference::Message& systemMessage);

// The engine's condensed summary, written without a model: one "Turns A-B: <text>" line per summary, cut to
// maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers every summary's range. Throws ContractError for none.
core::Summary CondenseFallback(const std::vector<core::Summary>& summaries, int maxTokens);

// Condenses every summary into one covering their whole range, with one call on CondensePrompt. When the model
// writes nothing, the summary is CondenseFallback(summaries, spec.maxTokens).
// Returns nullopt when cancelled. Throws ContractError for no summaries or a non-system message, and
// InferenceError from the backend.
std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop);

} // namespace morpheus::turn
