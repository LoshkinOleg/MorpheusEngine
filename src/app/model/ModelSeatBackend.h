#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "app/model/ModelSeat.h"
#include "inference/Llm.h"

namespace morpheus::app {

// A backend through which the model seat answers the engine's model calls (the model seat design). Everything but
// Complete is forwarded to the real backend unchanged. Complete forwards too while the seat is off or the call's
// label is in Model mode; otherwise it publishes the call to the seat, waits for the owner's reply (in Draft mode
// after the real model has answered, as the draft), and returns the reply as the model's completion. Used on the
// engine worker thread like any backend; the seat is shared with the UI thread.
class ModelSeatBackend final : public inference::LlmBackend {
private:
    std::unique_ptr<inference::LlmBackend> _pInner = nullptr; // The real backend.
    std::shared_ptr<ModelSeat> _pSeat = nullptr;

public:
    // Throws ContractError when `pInner` or `pSeat` is null.
    ModelSeatBackend(std::unique_ptr<inference::LlmBackend> pInner, std::shared_ptr<ModelSeat> pSeat);

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    // A reply typed by the owner: its text is cut to spec.maxTokens and its thinking to spec.thinkingMaxTokens (and
    // dropped when that is 0), each at the longest prefix within the limit by the real tokenizer that ends on a UTF-8
    // character; stoppedAtLimit and thinkingStoppedAtLimit say whether they were cut. Its token counts are the real
    // counts of what is kept, the prompt counts as evaluated in full (none reused), and generateMs is the time the
    // request waited. A draft submitted unchanged is the model's own completion. The answer reaches onToken once,
    // whole. A stop or a Withdraw while waiting returns a completion with only `cancelled` set, which the caller
    // treats as any cancelled model call (a turn closes with TurnCancelled, a fold or condense stores nothing).
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;
};

} // namespace morpheus::app
