#include "turn/Folding.h"

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "context/Transcript.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

inference::Prompt SummarizePrompt(const inference::Message& systemMessage, const std::vector<core::TurnRecord>& turns)
{
    // The system message, then the turns as the transcript renders them (stable, so cached), then the instruction.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (inference::Message& message : context::RenderTranscript(turns)) {
        prompt.messages.push_back(std::move(message));
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, std::string(SUMMARIZE_INSTRUCTION)});
    return prompt;
}

core::Summary SummarizeFallback(const std::vector<core::TurnRecord>& turns, int maxTokens)
{
    MORPHEUS_REQUIRE(!turns.empty() && maxTokens > 0, "SummarizeFallback: needs at least one turn and a token cap");
    // Each turn's input and mechanics line: the facts the engine knows without a model.
    std::string text;
    for (const core::TurnRecord& turn : turns) {
        const std::string mechanics = context::RenderMechanicsLine(turn);
        text += std::format("{}Turn {}: {}{}{}", text.empty() ? "" : "\n", turn.number, turn.input,
                            mechanics.empty() ? "" : " ", mechanics);
    }
    // Cut to about the summary's token cap.
    text = core::CutUtf8(text, static_cast<std::size_t>(maxTokens) * FALLBACK_BYTES_PER_TOKEN);
    return core::Summary{turns.front().number, turns.back().number, std::move(text)};
}

std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<core::TurnRecord>& turns,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !turns.empty(),
                     "SummarizeTurns: needs the system message and at least one turn");
    const inference::Completion completion = backend.Complete(SummarizePrompt(systemMessage, turns), spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }

    // The model's summary, trimmed. When it wrote nothing, the engine writes a factual one so folding still
    // makes progress.
    std::string text = core::Trim(completion.text);
    if (text.empty()) {
        return SummarizeFallback(turns, spec.maxTokens);
    }
    return core::Summary{turns.front().number, turns.back().number, std::move(text)};
}

inference::Prompt CondensePrompt(const inference::Message& systemMessage)
{
    // The system message already holds every summary (L2), so only the instruction follows it.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    prompt.stableCount = 1;
    prompt.messages.push_back({inference::Role::User, std::string(CONDENSE_INSTRUCTION)});
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
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !summaries.empty(),
                     "CondenseSummaries: needs the system message and at least one summary");
    const inference::Completion completion = backend.Complete(CondensePrompt(systemMessage), spec, {}, stop);
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
