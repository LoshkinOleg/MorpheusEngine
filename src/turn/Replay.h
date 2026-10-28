#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Dice.h"
#include "core/MemoryMode.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Which call a completion answers, told apart the way the session makes them: the lookup grammar names "lookups",
// any other grammar is the resolve call's, and a grammar-free call ending with the summarize or condense
// instruction is a summary; the rest is narration.
core::CallKind CallKindOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec);

// Fixture spelling of a call kind: "lookup", "resolve", "narrate" or "summarize".
std::string CallKindName(core::CallKind kind);

// One player command in a replay fixture.
struct ReplayStep {
    enum class Action { Input, Undo, Regenerate };
    Action action = Action::Input;
    std::string input = {}; // Input only.
};

// Wraps a backend during play and records what a replay fixture needs (architecture spec, "Testing"): the text of
// every completed call, by kind, in order. Cancelled calls are not recorded. Everything else is forwarded.
class RecordingBackend final : public inference::LlmBackend {
private:
    inference::LlmBackend* _pInner = nullptr; // Not owned.
    std::map<core::CallKind, std::vector<std::string>> _replies = {};

public:
    explicit RecordingBackend(inference::LlmBackend& inner);

    const std::map<core::CallKind, std::vector<std::string>>& Replies() const;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;
};

// What a replay checks of one committed turn: {"number", "input", "checks", "applied", "rejected", "narration"},
// in the store's JSON forms.
nlohmann::json TurnResultJson(const core::TurnRecord& record);

// A replay fixture (format in tests/data/replays and the M3 plan): the world folder (relative to the repo root),
// seed, memory mode, the steps played, every reply by kind, and what the session committed.
nlohmann::json MakeReplayFixture(const std::string& worldFolder, std::uint64_t seed, core::MemoryMode mode,
                                 const std::vector<ReplayStep>& steps, const RecordingBackend& recording,
                                 const std::vector<core::TurnRecord>& history, const nlohmann::json& finalState);

} // namespace morpheus::turn
