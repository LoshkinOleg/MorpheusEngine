#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

struct llama_context;
struct llama_model;
struct llama_vocab;

namespace morpheus::llama {

struct LlamaConfig {
    std::filesystem::path chatModel = {};
    std::filesystem::path embeddingModel = {};
    std::string embeddingQueryPrefix = {};    // Prepended for EmbedPurpose::Query.
    std::string embeddingDocumentPrefix = {}; // Prepended for EmbedPurpose::Document.
    int contextSize = 8192;
    int gpuLayers = -1;                       // -1 puts every layer on the GPU.
    int embeddingContextSize = 2048;          // Longest text (in tokens) that can be embedded.
};

// llama.cpp in-process: a chat model for generation and token counting, plus a sentence-embedding model.
// The chat KV cache is reused by longest common token prefix across calls. Not thread-safe: one thread owns it.
class LlamaCppBackend final : public inference::LlmBackend {
public:
    struct ModelDeleter {
        void operator()(llama_model* pModel) const;
    };
    struct ContextDeleter {
        void operator()(llama_context* pContext) const;
    };

private:
    LlamaConfig _config = {};
    std::unique_ptr<llama_model, ModelDeleter> _pChatModel = nullptr;
    std::unique_ptr<llama_context, ContextDeleter> _pChatContext = nullptr;
    std::unique_ptr<llama_model, ModelDeleter> _pEmbedModel = nullptr;
    std::unique_ptr<llama_context, ContextDeleter> _pEmbedContext = nullptr;
    std::vector<std::int32_t> _cachedTokens = {}; // Exactly the tokens in the chat KV cache (sequence 0), in order.

public:
    // Loads both models and creates their contexts. Throws InferenceError if either fails.
    explicit LlamaCppBackend(LlamaConfig config);
    ~LlamaCppBackend() override;
    LlamaCppBackend(const LlamaCppBackend&) = delete;
    LlamaCppBackend& operator=(const LlamaCppBackend&) = delete;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;

private:
    // The prompt rendered through the chat model's template, ending where the assistant's reply begins.
    std::string RenderPrompt(const inference::Prompt& prompt) const;

    // RenderPrompt tokenized the way Complete evaluates it.
    std::vector<std::int32_t> TokenizePrompt(const inference::Prompt& prompt) const;

    // Decodes `tokens` into the chat cache right after _cachedTokens, then appends them to _cachedTokens.
    // On failure the cache is cleared (it may hold part of the batch) and InferenceError is thrown.
    void DecodeChat(std::span<const std::int32_t> tokens, bool wantLogits);

    static std::vector<std::int32_t> Tokenize(const llama_vocab* pVocab, std::string_view text, bool addSpecial,
                                              bool parseSpecial);
};

} // namespace morpheus::llama
