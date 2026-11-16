#include "app/model/ModelSeatBackend.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

#include "core/Errors.h"

namespace morpheus::app {

ModelSeatBackend::ModelSeatBackend(std::unique_ptr<inference::LlmBackend> pInner, std::shared_ptr<ModelSeat> pSeat)
    : _pInner(std::move(pInner))
    , _pSeat(std::move(pSeat))
{
    MORPHEUS_REQUIRE(_pInner != nullptr, "ModelSeatBackend: no backend to wrap (the backend factory returned none)");
    MORPHEUS_REQUIRE(_pSeat != nullptr, "ModelSeatBackend: no model seat");
}

int ModelSeatBackend::CountTokens(std::string_view text)
{
    return _pInner->CountTokens(text);
}

int ModelSeatBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return _pInner->CountPromptTokens(prompt);
}

int ModelSeatBackend::ContextSize() const
{
    return _pInner->ContextSize();
}

inference::Completion ModelSeatBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                 const inference::TokenCallback& onToken, std::stop_token stop)
{
    // 1. With the seat off, or this kind of call in Model mode, the real model answers, as without the seat.
    if (!_pSeat->Enabled()) {
        return _pInner->Complete(prompt, spec, onToken, stop);
    }
    const SeatLabel label = SeatLabelOf(spec);
    const SeatMode mode = _pSeat->ModeOf(label);
    if (mode == SeatMode::Model) {
        return _pInner->Complete(prompt, spec, onToken, stop);
    }
    // The seat answers: the interface's contract holds for it too.
    MORPHEUS_REQUIRE(spec.maxTokens > 0, "Complete: maxTokens must be positive");
    MORPHEUS_REQUIRE(spec.thinkingMaxTokens >= 0, "Complete: thinkingMaxTokens must not be negative");

    // 2. Draft: the real model answers first, with no token callback, so nothing reaches the Play window until the
    // owner submits. A draft cancelled midway ends the call as a cancelled model call.
    std::optional<inference::Completion> draft = {};
    if (mode == SeatMode::Draft) {
        draft = _pInner->Complete(prompt, spec, {}, stop);
        if (draft->cancelled) {
            return std::move(*draft);
        }
    }

    // 3. Publish the request and wait for the owner's reply, or for the stop token.
    using Clock = std::chrono::steady_clock;
    const Clock::time_point published = Clock::now();
    const std::optional<SeatReply> reply =
        _pSeat->Await(SeatRequest{.label = label, .prompt = prompt, .spec = spec, .draft = draft}, *_pInner, stop);
    if (!reply) {
        // Stopped or withdrawn: as a model call cancelled midway, which the caller discards.
        inference::Completion cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    const double waitedMs = std::chrono::duration<double, std::milli>(Clock::now() - published).count();

    // 4. The completion. Thinking counts only when the call has a thinking budget.
    const bool thinks = spec.thinkingMaxTokens > 0;
    inference::Completion completion;
    if (draft && reply->text == draft->text && (!thinks || reply->thinking == draft->thinking)) {
        // The draft submitted unchanged: the model's own completion, with its token counts and timings.
        completion = std::move(*draft);
    } else {
        // The longest prefix of `text` within `maxTokens` tokens of the real tokenizer that ends on a UTF-8
        // character boundary; `cut` says whether anything was dropped.
        auto keepWithin = [this](const std::string& text, int maxTokens, bool& cut) -> std::string {
            cut = _pInner->CountTokens(text) > maxTokens;
            if (!cut) {
                return text;
            }
            // The places a cut may fall: the start, each byte that begins a UTF-8 character, and the end.
            std::vector<std::size_t> bounds = {0};
            for (std::size_t at = 1; at < text.size(); ++at) {
                if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80) {
                    bounds.push_back(at);
                }
            }
            bounds.push_back(text.size());
            // Bisect: the prefix ending at bounds[low] always fits (low starts at the empty prefix), the one ending
            // at bounds[high] never does (high starts at the whole text).
            std::size_t low = 0;
            std::size_t high = bounds.size() - 1;
            while (high - low > 1) {
                const std::size_t middle = low + (high - low) / 2;
                if (_pInner->CountTokens(std::string_view(text).substr(0, bounds[middle])) <= maxTokens) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return text.substr(0, bounds[low]);
        };
        completion.text = keepWithin(reply->text, spec.maxTokens, completion.stoppedAtLimit);
        completion.tokensGenerated = _pInner->CountTokens(completion.text);
        if (thinks) {
            completion.thinking = keepWithin(reply->thinking, spec.thinkingMaxTokens, completion.thinkingStoppedAtLimit);
            completion.thinkingTokens = _pInner->CountTokens(completion.thinking);
        }
        // The whole prompt counts as evaluated; generating took the time the request waited.
        completion.promptTokens = _pInner->CountPromptTokens(prompt);
        completion.tokensEvaluated = completion.promptTokens;
        completion.tokensReused = 0;
        completion.generateMs = waitedMs;
    }

    // 5. The answer reaches the Play window in one piece, as if it had streamed.
    if (onToken && !completion.text.empty()) {
        onToken(completion.text);
    }
    return completion;
}

std::vector<std::vector<float>> ModelSeatBackend::Embed(const std::vector<std::string>& texts,
                                                        inference::EmbedPurpose purpose)
{
    return _pInner->Embed(texts, purpose);
}

std::string ModelSeatBackend::EmbeddingModelId() const
{
    return _pInner->EmbeddingModelId();
}

std::vector<std::uint8_t> ModelSeatBackend::SaveState()
{
    return _pInner->SaveState();
}

void ModelSeatBackend::RestoreState(std::span<const std::uint8_t> state)
{
    _pInner->RestoreState(state);
}

} // namespace morpheus::app
