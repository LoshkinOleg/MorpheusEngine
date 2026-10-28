#include "turn/Replay.h"

#include "core/Errors.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

core::CallKind CallKindOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec)
{
    // A grammar means a structured call: the lookup grammar is the only one that names "lookups".
    if (!spec.grammar.empty()) {
        return spec.grammar.find("lookups") != std::string::npos ? core::CallKind::Lookup : core::CallKind::Resolve;
    }
    // Without a grammar, the closing instruction tells a summary from narration.
    if (!prompt.messages.empty()) {
        const std::string& last = prompt.messages.back().content;
        if (last == SUMMARIZE_INSTRUCTION || last == CONDENSE_INSTRUCTION) {
            return core::CallKind::Summarize;
        }
    }
    return core::CallKind::Narrate;
}

std::string CallKindName(core::CallKind kind)
{
    switch (kind) {
    case core::CallKind::Lookup:
        return "lookup";
    case core::CallKind::Resolve:
        return "resolve";
    case core::CallKind::Narrate:
        return "narrate";
    case core::CallKind::Summarize:
        return "summarize";
    }
    throw core::ContractError("CallKindName: unhandled CallKind");
}

RecordingBackend::RecordingBackend(inference::LlmBackend& inner)
    : _pInner(&inner)
{
}

const std::map<core::CallKind, std::vector<std::string>>& RecordingBackend::Replies() const
{
    return _replies;
}

int RecordingBackend::CountTokens(std::string_view text)
{
    return _pInner->CountTokens(text);
}

int RecordingBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return _pInner->CountPromptTokens(prompt);
}

int RecordingBackend::ContextSize() const
{
    return _pInner->ContextSize();
}

inference::Completion RecordingBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                 const inference::TokenCallback& onToken, std::stop_token stop)
{
    inference::Completion completion = _pInner->Complete(prompt, spec, onToken, stop);
    // A cancelled call is discarded by the session, so a replay never sees it.
    if (!completion.cancelled) {
        _replies[CallKindOf(prompt, spec)].push_back(completion.text);
    }
    return completion;
}

std::vector<std::vector<float>> RecordingBackend::Embed(const std::vector<std::string>& texts,
                                                        inference::EmbedPurpose purpose)
{
    return _pInner->Embed(texts, purpose);
}

std::string RecordingBackend::EmbeddingModelId() const
{
    return _pInner->EmbeddingModelId();
}

std::vector<std::uint8_t> RecordingBackend::SaveState()
{
    return _pInner->SaveState();
}

void RecordingBackend::RestoreState(std::span<const std::uint8_t> state)
{
    _pInner->RestoreState(state);
}

nlohmann::json TurnResultJson(const core::TurnRecord& record)
{
    nlohmann::json checks = nlohmann::json::array();
    for (const core::CheckOutcome& check : record.checks) {
        checks.push_back(core::CheckOutcomeToJson(check));
    }
    nlohmann::json applied = nlohmann::json::array();
    for (const core::AppliedChange& change : record.applied) {
        applied.push_back(core::AppliedChangeToJson(change));
    }
    nlohmann::json rejected = nlohmann::json::array();
    for (const core::RejectedChange& change : record.rejected) {
        rejected.push_back(core::RejectedChangeToJson(change));
    }
    return {{"number", record.number}, {"input", record.input}, {"checks", checks},
            {"applied", applied},      {"rejected", rejected},  {"narration", record.narration}};
}

nlohmann::json MakeReplayFixture(const std::string& worldFolder, std::uint64_t seed, core::MemoryMode mode,
                                 const std::vector<ReplayStep>& steps, const RecordingBackend& recording,
                                 const std::vector<core::TurnRecord>& history, const nlohmann::json& finalState)
{
    nlohmann::json stepsJson = nlohmann::json::array();
    for (const ReplayStep& step : steps) {
        switch (step.action) {
        case ReplayStep::Action::Input:
            stepsJson.push_back({{"input", step.input}});
            break;
        case ReplayStep::Action::Undo:
            stepsJson.push_back({{"undo", true}});
            break;
        case ReplayStep::Action::Regenerate:
            stepsJson.push_back({{"regenerate", true}});
            break;
        }
    }
    nlohmann::json replies = nlohmann::json::object();
    for (const auto& [kind, texts] : recording.Replies()) {
        replies[CallKindName(kind)] = texts;
    }
    nlohmann::json turns = nlohmann::json::array();
    for (const core::TurnRecord& record : history) {
        turns.push_back(TurnResultJson(record));
    }
    return {{"world", worldFolder},
            {"seed", seed},
            {"memory_mode", mode == core::MemoryMode::Engine ? "engine" : "assisted"},
            {"steps", stepsJson},
            {"replies", replies},
            {"expected", {{"turns", turns}, {"final_state", finalState}}}};
}

} // namespace morpheus::turn
