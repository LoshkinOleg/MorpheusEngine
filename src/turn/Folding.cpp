#include "turn/Folding.h"

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"

namespace morpheus::turn {

inference::Prompt SummarizePrompt(const inference::Message& systemMessage,
                                  const std::vector<context::TranscriptEntry>& entries, std::string_view instruction)
{
    // The system message, then each turn in full (stable, so cached), then the instruction. Summaries are written
    // from the full narration, digests or not.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (const context::TranscriptEntry& entry : entries) {
        for (const inference::Message& message : entry.foldMessages) {
            prompt.messages.push_back(message);
        }
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, std::string(instruction)});
    return prompt;
}

core::Summary SummarizeFallback(const std::vector<context::TranscriptEntry>& entries, int maxTokens)
{
    MORPHEUS_REQUIRE(!entries.empty() && maxTokens > 0, "SummarizeFallback: needs at least one entry and a token cap");
    // Each turn's fact: what the engine knows without a model.
    std::string text;
    for (const context::TranscriptEntry& entry : entries) {
        text += std::format("{}Turn {}: {}", text.empty() ? "" : "\n", entry.turn, entry.fact);
    }
    // Cut to about the summary's token cap.
    text = core::CutUtf8(text, static_cast<std::size_t>(maxTokens) * FALLBACK_BYTES_PER_TOKEN);
    return core::Summary{entries.front().turn, entries.back().turn, std::move(text)};
}

std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<context::TranscriptEntry>& entries,
                                            std::string_view instruction, const inference::GenerationSpec& spec,
                                            inference::LlmBackend& backend, std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !entries.empty(),
                     "SummarizeTurns: needs the system message and at least one entry");
    const inference::Completion completion =
        backend.Complete(SummarizePrompt(systemMessage, entries, instruction), spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }

    // The model's summary, trimmed. When it wrote nothing, the engine writes a factual one so folding still
    // makes progress.
    std::string text = core::Trim(completion.text);
    if (text.empty()) {
        return SummarizeFallback(entries, spec.maxTokens);
    }
    return core::Summary{entries.front().turn, entries.back().turn, std::move(text)};
}

inference::Prompt CondensePrompt(const inference::Message& systemMessage, std::string_view message)
{
    // The system message, then the caller's message: the owned game's system message already holds every summary
    // (L2), so its message is only the instruction.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    prompt.stableCount = 1;
    prompt.messages.push_back({inference::Role::User, std::string(message)});
    return prompt;
}

core::Summary CondenseFallback(const std::vector<core::Summary>& summaries, int maxTokens)
{
    MORPHEUS_REQUIRE(!summaries.empty() && maxTokens > 0, "CondenseFallback: needs at least one summary and a token cap");
    // Every summary's text under its range, cut to about the condensed summary's token cap.
    std::string text;
    for (const core::Summary& summary : summaries) {
        text += std::format("{}Turns {}-{}: {}", text.empty() ? "" : "\n", summary.fromTurn, summary.toTurn,
                            summary.text);
    }
    text = core::CutUtf8(text, static_cast<std::size_t>(maxTokens) * FALLBACK_BYTES_PER_TOKEN);
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, std::move(text)};
}

std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries, std::string_view message,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !summaries.empty(),
                     "CondenseSummaries: needs the system message and at least one summary");
    const inference::Completion completion = backend.Complete(CondensePrompt(systemMessage, message), spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }

    // The model's summary, trimmed. When it wrote nothing, the engine writes one from the summaries it was given.
    std::string text = core::Trim(completion.text);
    if (text.empty()) {
        return CondenseFallback(summaries, spec.maxTokens);
    }
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, std::move(text)};
}

} // namespace morpheus::turn
