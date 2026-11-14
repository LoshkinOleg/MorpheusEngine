#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

namespace morpheus::llamaserver {

// Where the models are and what Morpheus plans for. The callers build it from core::Settings.
struct LlamaServerConfig {
    std::string chatUrl = {};                 // "http://host:port" of the chat model's llama-server.
    std::string embeddingUrl = {};            // "http://host:port" of the embedding model's llama-server (--embedding).
    int slot = 0;                             // The server slot of calls whose GenerationSpec::slot is -1.
    int contextSize = 0;                      // settings.json's context_size: ContextSize(), what the budgets use.
    std::string embeddingQueryPrefix = {};    // Prepended for EmbedPurpose::Query.
    std::string embeddingDocumentPrefix = {}; // Prepended for EmbedPurpose::Document.
};

// The models through llama-server over HTTP (cpp-httplib), so one model host can serve several consumers. Token
// counts come from the chat server's POST /tokenize; prompts are rendered by its POST /apply-template around content
// markers and tokenized piece by piece, contents as plain text, exactly as LlamaCppBackend does, and sent to POST
// /completion as token ids on a slot with cache_prompt; vectors come from the embedding server's POST /embedding.
//
// Thinking models (the chat template has the enable_thinking switch): as LlamaCppBackend, the backend writes the
// thinking block's markers itself. A call that thinks is two /completion calls on one slot: the block, which stops at
// "</think>" or at its budget, then the answer, whose prompt extends the first one's, so the server reuses its cache.
//
// Not thread-safe: one thread owns it. Several backends may talk to one server at once, each on its own slot.
class LlamaServerBackend final : public inference::LlmBackend {
private:
    // The thinking block's texts as the chat server tokenizes them, as LlamaCppBackend::ThinkingMarkers.
    struct ThinkingMarkers {
        std::vector<std::int32_t> open = {};     // inference::THINKING_OPEN_TEXT, special tokens parsed.
        std::vector<std::int32_t> skip = {};     // inference::THINKING_SKIP_TEXT, special tokens parsed.
        std::vector<std::int32_t> close = {};    // inference::CLOSE_THINKING_TEXT, special tokens parsed.
        std::vector<std::int32_t> end = {};      // inference::THINKING_END_TEXT, special tokens parsed.
        std::vector<std::int32_t> afterEnd = {}; // inference::THINKING_AFTER_END_TEXT, as plain text.
    };

    LlamaServerConfig _config = {};
    std::optional<ThinkingMarkers> _thinking = {}; // Set when the chat template has the enable_thinking switch.
    std::optional<std::int32_t> _bos = {};         // The BOS token the chat model adds at a prompt's start, if any.
    std::string _embeddingModelId = {};            // The embedding server's model file name, without its extension.

public:
    // Reads both servers' GET /props: the chat server's context per slot, which must hold config.contextSize, and its
    // chat template (whether the model thinks); the embedding server's model file. Then tokenizes the thinking texts
    // (thinking models) and an empty text with add_special (the BOS the model adds, if any). Throws InferenceError
    // naming the URL for a URL that is not http://host:port, an unreachable server, an HTTP error, an unexpected
    // answer, or slots smaller than config.contextSize. Throws ContractError for a context size below 1 or a slot
    // below 0.
    explicit LlamaServerBackend(LlamaServerConfig config);

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    // Not supported: llama-server keeps its slots' caches itself. Both throw InferenceError.
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;

private:
    // The prompt's tokens as Complete sends them without thinking: the chat server's template rendered around content
    // markers (inference::ContentMarker), each template piece tokenized with special tokens parsed (the first with
    // BOS when the model adds one, never twice), each content as plain text, then a thinking model's empty block.
    // Throws InferenceError if the chat template drops or repeats a content.
    std::vector<std::int32_t> TokenizePrompt(const inference::Prompt& prompt) const;

    // The chat server's tokens for `text`: with addSpecial, BOS where the model wants one; with parseSpecial,
    // control-token text becomes control tokens.
    std::vector<std::int32_t> ServerTokens(std::string_view text, bool addSpecial, bool parseSpecial) const;
};

} // namespace morpheus::llamaserver
