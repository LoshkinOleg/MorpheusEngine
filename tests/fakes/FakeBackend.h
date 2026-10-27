#pragma once

#include "core/Errors.h"
#include "inference/Llm.h"

namespace morpheus::test {

// Deterministic stand-in for context tests. One token per 4 bytes (rounded up), plus 4 template tokens per
// message. Generation, embeddings and state are not used by those tests and throw if called.
class FakeBackend final : public inference::LlmBackend {
private:
    int _contextSize = 0;

public:
    explicit FakeBackend(int contextSize)
        : _contextSize(contextSize)
    {
    }

    int CountTokens(std::string_view text) override
    {
        return static_cast<int>((text.size() + 3) / 4);
    }

    int CountPromptTokens(const inference::Prompt& prompt) override
    {
        int total = 0;
        for (const inference::Message& message : prompt.messages) {
            total += CountTokens(message.content) + 4;
        }
        return total;
    }

    int ContextSize() const override
    {
        return _contextSize;
    }

    inference::Completion Complete(const inference::Prompt&, const inference::GenerationSpec&,
                                   const inference::TokenCallback&, std::stop_token) override
    {
        throw core::ContractError("FakeBackend::Complete is not available");
    }

    std::vector<std::vector<float>> Embed(const std::vector<std::string>&, inference::EmbedPurpose) override
    {
        throw core::ContractError("FakeBackend::Embed is not available");
    }

    std::string EmbeddingModelId() const override
    {
        throw core::ContractError("FakeBackend::EmbeddingModelId is not available");
    }

    std::vector<std::uint8_t> SaveState() override
    {
        throw core::ContractError("FakeBackend::SaveState is not available");
    }

    void RestoreState(std::span<const std::uint8_t>) override
    {
        throw core::ContractError("FakeBackend::RestoreState is not available");
    }
};

} // namespace morpheus::test
