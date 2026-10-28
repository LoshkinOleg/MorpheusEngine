#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/ContentHash.h"
#include "core/Errors.h"
#include "fakes/FakeBackend.h"

namespace morpheus::test {

// A backend for turn-layer tests. Tokens are counted like FakeBackend. Complete enforces the context like the real
// backend (InferenceError when the prompt plus maxTokens is over ContextSize()), then asks the responder for the reply,
// streams it in pieces of about 4 bytes that never split a UTF-8 character (stopping early when `stop` is
// requested), and records the call. Embed maps a text to a bag of words (lower-cased [a-z0-9] runs hashed into
// EMBED_DIMS dimensions, L2-normalized), so texts sharing words score higher. Save/RestoreState keep nothing.
class ScriptedBackend final : public FakeBackend {
public:
    using Responder = std::function<std::string(const inference::Prompt&, const inference::GenerationSpec&)>;

    struct Call {
        inference::Prompt prompt = {};
        inference::GenerationSpec spec = {};
    };

    static constexpr std::size_t EMBED_DIMS = 64;

private:
    Responder _responder = {};
    std::vector<Call> _calls = {};
    int _embeddedTexts = 0;
    std::size_t _embedLimit = 0; // Longest text Embed accepts, in bytes; 0 means no limit.
    std::chrono::milliseconds _pieceDelay = std::chrono::milliseconds(0);

public:
    explicit ScriptedBackend(int contextSize, Responder responder = {})
        : FakeBackend(contextSize)
        , _responder(std::move(responder))
    {
    }

    const std::vector<Call>& Calls() const
    {
        return _calls;
    }

    // Texts embedded so far, across calls.
    int EmbeddedTexts() const
    {
        return _embeddedTexts;
    }

    // Embed throws InferenceError for a text longer than `bytes` (an embedding context that is too small).
    void SetEmbedLimit(std::size_t bytes)
    {
        _embedLimit = bytes;
    }

    // Sleeps before each streamed piece, so tests can cancel mid-reply.
    void SetPieceDelay(std::chrono::milliseconds delay)
    {
        _pieceDelay = delay;
    }

    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override
    {
        MORPHEUS_REQUIRE(static_cast<bool>(_responder), "ScriptedBackend: no responder for Complete");
        MORPHEUS_REQUIRE(spec.maxTokens > 0, "Complete: maxTokens must be positive");
        // The context limit, checked like LlamaCppBackend::Complete checks it: before anything else happens.
        inference::Completion completion;
        completion.promptTokens = CountPromptTokens(prompt);
        if (spec.maxTokens > ContextSize() - completion.promptTokens) {
            throw inference::InferenceError(std::format("prompt ({} tokens) plus max_tokens ({}) exceeds the context size ({})",
                                                        completion.promptTokens, spec.maxTokens, ContextSize()));
        }
        _calls.push_back({prompt, spec});
        const std::string reply = _responder(prompt, spec);

        completion.tokensEvaluated = completion.promptTokens;
        std::size_t at = 0;
        while (at < reply.size()) {
            if (_pieceDelay.count() > 0) {
                std::this_thread::sleep_for(_pieceDelay);
            }
            if (stop.stop_requested()) {
                completion.cancelled = true;
                return completion;
            }
            std::size_t end = std::min(at + 4, reply.size());
            while (end < reply.size() && (static_cast<unsigned char>(reply[end]) & 0xC0) == 0x80) {
                ++end; // Never end a piece inside a UTF-8 character.
            }
            const std::string_view piece = std::string_view(reply).substr(at, end - at);
            completion.text += piece;
            ++completion.tokensGenerated;
            if (onToken) {
                onToken(piece);
            }
            at = end;
        }
        return completion;
    }

    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose) override
    {
        std::vector<std::vector<float>> vectors;
        for (const std::string& text : texts) {
            if (_embedLimit != 0 && text.size() > _embedLimit) {
                throw inference::InferenceError(std::format("text of {} bytes is over the embedding limit", text.size()));
            }
            ++_embeddedTexts;
            std::vector<float> vector(EMBED_DIMS, 0.0f);
            std::string word;
            auto flush = [&] {
                if (!word.empty()) {
                    core::ContentHash hash;
                    hash.Add(word);
                    vector[hash.Value() % EMBED_DIMS] += 1.0f;
                    word.clear();
                }
            };
            for (const char c : text) {
                const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
                    word += lower;
                } else {
                    flush();
                }
            }
            flush();
            float norm = 0.0f;
            for (const float value : vector) {
                norm += value * value;
            }
            if (norm > 0.0f) {
                const float length = std::sqrt(norm);
                for (float& value : vector) {
                    value /= length;
                }
            }
            vectors.push_back(std::move(vector));
        }
        return vectors;
    }

    std::string EmbeddingModelId() const override
    {
        return "scripted-embed";
    }

    std::vector<std::uint8_t> SaveState() override
    {
        return {};
    }

    void RestoreState(std::span<const std::uint8_t>) override
    {
    }
};

} // namespace morpheus::test
