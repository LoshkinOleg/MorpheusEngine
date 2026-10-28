#pragma once

#include <optional>
#include <stop_token>
#include <vector>

#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Summarizes `turns` (the oldest unfolded turns, in order) with one call: `systemMessage` (the resolve prompt's
// system message, L0-L2), the turns as transcript messages, then SUMMARIZE_INSTRUCTION. That prompt starts like the
// resolve prompt, so the KV cache serves most of it. The summary covers the turns' range; its text is trimmed.
// Returns nullopt when `stop` cancels the call. Throws ContractError for no turns or a non-system message.
std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<core::TurnRecord>& turns,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop);

// Condenses every summary into one covering their whole range: `systemMessage` (whose L2 holds them), then
// CONDENSE_INSTRUCTION. Returns nullopt when cancelled. Throws ContractError for no summaries.
std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop);

} // namespace morpheus::turn
