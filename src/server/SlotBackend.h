#pragma once

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

namespace morpheus::server {

// An LlmBackend that forwards every call to another and puts its calls on a chosen server slot: Complete sets
// GenerationSpec::slot to the current slot when the call left it at -1 (the backend's default slot), and keeps a slot
// the call chose. A run's worker sets the slot before each job (the conversation's) and before folding (the upkeep
// slot), so HostedSession's calls land on the right slot without knowing about slots. Not thread-safe, like the
// backend it wraps.
class SlotBackend final : public inference::LlmBackend {
private:
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives this.
    int _slot = -1;                             // -1: the wrapped backend's default slot.

public:
    explicit SlotBackend(inference::LlmBackend& backend);

    // The slot of the calls that follow; -1 for the wrapped backend's default. Throws ContractError below -1.
    void SetSlot(int slot);
    int Slot() const;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
};

} // namespace morpheus::server
