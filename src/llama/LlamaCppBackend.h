#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

struct llama_context;
struct llama_model;
struct llama_vocab;

namespace morpheus::llama {

enum class LogLevel { Debug, Info, Warn, Error };

// Receives llama.cpp and ggml log text, on whichever thread logs, one call per piece llama.cpp emits.
// The sink must not throw: it is called from llama.cpp's C code.
using LogSink = std::function<void(LogLevel level, std::string_view text)>;

// Routes llama.cpp's process-wide log to `sink`, replacing the previous one. An empty sink restores the default,
// which writes warnings and errors to stderr. Continuation pieces keep the level of the line they continue.
// Thread-safe. The sink runs under a lock: it must not call SetLogSink.
void SetLogSink(LogSink sink);

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
//
// Thinking models: a chat model whose template has the enable_thinking switch (Qwen3 and later) reasons in a
// "<think>...</think>" block before it answers. llama_chat_apply_template renders such templates as plain chatml,
// without the block, so the backend writes it itself, as the model's own template would: every prompt ends with an
// empty block ("<think>\n\n</think>\n\n", thinking off), or with the open marker when the call may think. A block
// that reaches its budget is closed with CLOSE_THINKING_TEXT.
class LlamaCppBackend final : public inference::LlmBackend {
public:
    struct ModelDeleter {
        void operator()(llama_model* pModel) const;
    };
    struct ContextDeleter {
        void operator()(llama_context* pContext) const;
    };

    // The thinking block's markers, tokenized with the chat model's vocabulary.
    struct ThinkingMarkers {
        std::vector<std::int32_t> open = {};     // "<think>\n": the prompt's end when the call may think.
        std::vector<std::int32_t> skip = {};     // "<think>\n\n</think>\n\n": the prompt's end otherwise.
        std::vector<std::int32_t> close = {};    // CLOSE_THINKING_TEXT: ends a block that ran out of budget.
        std::vector<std::int32_t> afterEnd = {}; // "\n\n": follows "</think>" when the model ends the block itself.
        std::int32_t endToken = 0;               // "</think>", a single token.
    };

    // Closes a thinking block that reached its budget: a nudge to answer from the thinking so far (the wording
    // Qwen documents for budget-limited thinking), then the end marker and the blank line before the answer.
    static constexpr const char* CLOSE_THINKING_TEXT =
        "\n\nConsidering the limited time, I have to give the solution based on the thinking directly now.\n</think>\n\n";

private:
    LlamaConfig _config = {};
    std::unique_ptr<llama_model, ModelDeleter> _pChatModel = nullptr;
    std::unique_ptr<llama_context, ContextDeleter> _pChatContext = nullptr;
    std::unique_ptr<llama_model, ModelDeleter> _pEmbedModel = nullptr;
    std::unique_ptr<llama_context, ContextDeleter> _pEmbedContext = nullptr;
    std::vector<std::int32_t> _cachedTokens = {}; // Exactly the tokens in the chat KV cache (sequence 0), in order.
    std::optional<ThinkingMarkers> _thinking = {}; // Set when the chat model is a thinking model.

public:
    // Loads both models and creates their contexts. Throws InferenceError if either fails, or if the chat template
    // has a thinking switch but "<think>" and "</think>" are not single tokens.
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

    // RenderPrompt tokenized the way Complete evaluates it: template text with special tokens parsed, message
    // contents as plain text, whatever bytes they hold. A thinking model's prompt ends with the empty thinking block
    // (ThinkingMarkers::skip). Throws InferenceError if the chat template drops or repeats a content.
    std::vector<std::int32_t> TokenizePrompt(const inference::Prompt& prompt) const;

    // Decodes `tokens` into the chat cache right after _cachedTokens, then appends them to _cachedTokens.
    // On failure the cache is cleared (it may hold part of the batch) and InferenceError is thrown.
    void DecodeChat(std::span<const std::int32_t> tokens, bool wantLogits);

    static std::vector<std::int32_t> Tokenize(const llama_vocab* pVocab, std::string_view text, bool addSpecial,
                                              bool parseSpecial);
};

} // namespace morpheus::llama
