#include "turn/Replay.h"

#include <iterator>

#include "core/Errors.h"
#include "turn/EngineText.h"
#include "turn/ResolveGrammar.h"

namespace morpheus::turn {

core::CallKind CallKindOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec)
{
    // A grammar means a structured call. The lookup grammar is told by its root rule (its first line), which no
    // resolve grammar shares; searching the whole text would trip on a world with a field named "lookups".
    if (!spec.grammar.empty()) {
        static const std::string LOOKUP_ROOT = [] {
            const std::string lookup = BuildLookupGrammar();
            return lookup.substr(0, lookup.find('\n'));
        }();
        return spec.grammar.starts_with(LOOKUP_ROOT) ? core::CallKind::Lookup : core::CallKind::Resolve;
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

std::map<core::CallKind, std::vector<std::string>> RecordingBackend::Replies() const
{
    const std::lock_guard<std::mutex> lock(_mutex);
    return _replies;
}

std::map<core::CallKind, std::size_t> RecordingBackend::Mark() const
{
    const std::lock_guard<std::mutex> lock(_mutex);
    std::map<core::CallKind, std::size_t> mark;
    for (const auto& [kind, texts] : _replies) {
        mark[kind] = texts.size();
    }
    return mark;
}

void RecordingBackend::RollBack(const std::map<core::CallKind, std::size_t>& mark)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    // Summarize replies are never rolled back (see the header), so the mark's summarize count is ignored.
    // A mark can only be restored if nothing before it was lost.
    for (const auto& [kind, count] : mark) {
        if (kind == core::CallKind::Summarize) {
            continue;
        }
        const auto found = _replies.find(kind);
        MORPHEUS_REQUIRE(found != _replies.end() && found->second.size() >= count,
                         "RollBack: fewer '" + CallKindName(kind) + "' replies recorded than the mark holds");
    }
    // Truncate each other kind to its marked count; a kind the mark lacks had none, so it goes entirely.
    for (auto it = _replies.begin(); it != _replies.end();) {
        if (it->first == core::CallKind::Summarize) {
            ++it;
            continue;
        }
        const auto marked = mark.find(it->first);
        const std::size_t keep = marked == mark.end() ? 0 : marked->second;
        it->second.resize(keep);
        it = keep == 0 ? _replies.erase(it) : std::next(it);
    }
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
        const core::CallKind kind = CallKindOf(prompt, spec);
        const std::lock_guard<std::mutex> lock(_mutex); // Only the append is guarded, not the inner call above.
        _replies[kind].push_back(completion.text);
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
    // One copy, taken under the recording's lock, so the loop sees a consistent recording.
    const std::map<core::CallKind, std::vector<std::string>> recorded = recording.Replies();
    for (const auto& [kind, texts] : recorded) {
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
