#include "turn/Folding.h"

#include <string>
#include <utility>

#include "context/Transcript.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

namespace {

// The summary text from a completion: trimmed; a model that wrote nothing still leaves a marker, not a blank.
std::string SummaryText(const std::string& output)
{
    std::string text = core::Trim(output);
    return text.empty() ? std::string("(no summary)") : text;
}

} // namespace

std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<core::TurnRecord>& turns,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !turns.empty(),
                     "SummarizeTurns: needs the system message and at least one turn");
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (inference::Message& message : context::RenderTranscript(turns)) {
        prompt.messages.push_back(std::move(message));
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, std::string(SUMMARIZE_INSTRUCTION)});

    const inference::Completion completion = backend.Complete(prompt, spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }
    return core::Summary{turns.front().number, turns.back().number, SummaryText(completion.text)};
}

std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !summaries.empty(),
                     "CondenseSummaries: needs the system message and at least one summary");
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    prompt.stableCount = 1;
    prompt.messages.push_back({inference::Role::User, std::string(CONDENSE_INSTRUCTION)});

    const inference::Completion completion = backend.Complete(prompt, spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, SummaryText(completion.text)};
}

} // namespace morpheus::turn
