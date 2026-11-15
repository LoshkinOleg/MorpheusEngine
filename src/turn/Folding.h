#pragma once

#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

#include "context/Transcript.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Rough bytes per token; the fallback only needs to stay near the summary's token cap.
constexpr int FALLBACK_BYTES_PER_TOKEN = 4;

// The fold call's prompt: `systemMessage` (the prompt's system message, L0 up to L2), each entry's foldMessages, then
// `instruction` (the owned game's is SUMMARIZE_INSTRUCTION). stableCount ends after the entries. The memory upkeep
// measures it before a fold.
inference::Prompt SummarizePrompt(const inference::Message& systemMessage,
                                  const std::vector<context::TranscriptEntry>& entries, std::string_view instruction);

// The engine's summary of `entries`, written without a model: one "Turn N: <fact>" line per entry, cut to
// maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers the entries' range (the first's turn to the last's). Throws
// ContractError for no entries.
core::Summary SummarizeFallback(const std::vector<context::TranscriptEntry>& entries, int maxTokens);

// Summarizes `entries` (the oldest unfolded turns, in order) with one call on SummarizePrompt, which ends with
// `instruction`. That prompt starts like the turn's prompt, so the KV cache serves most of it. The summary covers the
// entries' range; its text is trimmed. When the model writes nothing, the summary is
// SummarizeFallback(entries, spec.maxTokens). Returns nullopt when `stop` cancels the call. Throws ContractError for no
// entries or a non-system message, and InferenceError from the backend (among others when the prompt does not fit the
// context: the caller measures it).
std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<context::TranscriptEntry>& entries,
                                            std::string_view instruction, const inference::GenerationSpec& spec,
                                            inference::LlmBackend& backend, std::stop_token stop);

// The condense call's prompt: `systemMessage`, then `message` as the user message. The owned game's system message
// holds the summaries (L2) and its message is CONDENSE_INSTRUCTION.
inference::Prompt CondensePrompt(const inference::Message& systemMessage, std::string_view message);

// The engine's condensed summary, written without a model: one "Turns A-B: <text>" line per summary, cut to
// maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers every summary's range. Throws ContractError for none.
core::Summary CondenseFallback(const std::vector<core::Summary>& summaries, int maxTokens);

// Condenses `summaries` into one covering their whole range, with one call on CondensePrompt(systemMessage, message).
// When the model writes nothing, the summary is CondenseFallback(summaries, spec.maxTokens). Returns nullopt when
// cancelled. Throws ContractError for no summaries or a non-system message, and InferenceError from the backend.
std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries, std::string_view message,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop);

} // namespace morpheus::turn
