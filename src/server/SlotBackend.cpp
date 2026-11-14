#include "server/SlotBackend.h"

#include <format>

#include "core/Errors.h"

namespace morpheus::server {

SlotBackend::SlotBackend(inference::LlmBackend& backend)
    : _pBackend(&backend)
{
}

void SlotBackend::SetSlot(int slot)
{
    MORPHEUS_REQUIRE(slot >= -1, std::format("SlotBackend::SetSlot: slot {} is below -1", slot));
    _slot = slot;
}

int SlotBackend::Slot() const
{
    return _slot;
}

int SlotBackend::CountTokens(std::string_view text)
{
    return _pBackend->CountTokens(text);
}

int SlotBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return _pBackend->CountPromptTokens(prompt);
}

int SlotBackend::ContextSize() const
{
    return _pBackend->ContextSize();
}

inference::Completion SlotBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                            const inference::TokenCallback& onToken, std::stop_token stop)
{
    // A call that chose no slot goes on the current one; a call that chose one keeps it.
    if (spec.slot != -1 || _slot == -1) {
        return _pBackend->Complete(prompt, spec, onToken, stop);
    }
    inference::GenerationSpec onSlot = spec;
    onSlot.slot = _slot;
    return _pBackend->Complete(prompt, onSlot, onToken, stop);
}

std::vector<std::uint8_t> SlotBackend::SaveState()
{
    return _pBackend->SaveState();
}

void SlotBackend::RestoreState(std::span<const std::uint8_t> state)
{
    _pBackend->RestoreState(state);
}

std::vector<std::vector<float>> SlotBackend::Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose)
{
    return _pBackend->Embed(texts, purpose);
}

std::string SlotBackend::EmbeddingModelId() const
{
    return _pBackend->EmbeddingModelId();
}

} // namespace morpheus::server
