#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/Thinking.h"
#include "inference/Llm.h"

namespace httplib {
class Client; // cpp-httplib, included only by LlamaServerBackend.cpp.
}

namespace morpheus::llamaserver {

// Where the models are and what Morpheus plans for. The callers build it from core::Settings.
struct LlamaServerConfig {
    std::string chatUrl = {};                 // "http://host:port" of the chat model's llama-server.
    std::string embeddingUrl = {};            // "http://host:port" of the embedding model's llama-server (--embedding).
    int slot = 0;                             // The server slot of calls whose GenerationSpec::slot is -1.
    int contextSize = 0;                      // settings.json's context_size: ContextSize(), what the budgets use.
    std::string embeddingQueryPrefix = {};    // Prepended for EmbedPurpose::Query.
    std::string embeddingDocumentPrefix = {}; // Prepended for EmbedPurpose::Document.
    core::ThinkingSetting thinking = {};      // settings.json's thinking_format; Auto reads the chat template.
};

// The models through llama-server over HTTP (cpp-httplib), so one model host can serve several consumers. Token
// counts come from the chat server's POST /tokenize; prompts are rendered by its POST /apply-template around content
// markers and tokenized piece by piece, contents as plain text, exactly as LlamaCppBackend does, and sent to POST
// /completion as token ids on a slot with cache_prompt; vectors come from the embedding server's POST /embedding.
// Each server is reached through one kept-alive connection, reused by every request (a new connection per request
// would cost a TCP connect each, about 2 s on Windows when "localhost" tries ::1 first); cpp-httplib reconnects when
// the server closed it, and a request that fails under a reused connection (llama-server closes one idle for 5 s, and
// can do so as a request leaves) is sent once more. The streaming /completion has a kept-alive client of its own,
// so that a cancel shuts down only its connection (the next call connects again); it is sent once more only when its
// reused connection (is_socket_open() before the request) failed before a byte of answer arrived. llama-server sends
// a streaming /completion's headers only after the first token, so that covers the whole prompt read: a failure or a
// read timeout during a long prompt read is retried, so a hung server takes about two read timeouts (about 1200 s) to
// fail, and a retried prompt runs again on the same slot, mostly from its cache.
//
// An HTTP 4xx answer to a request other than /completion (a text too long for the server's context among them) throws
// inference::InputRefused: the input was refused, and sending it again cannot help. A server that cannot be reached,
// fails (5xx) or answers what cannot be read throws a plain InferenceError.
//
// Thinking models (a resolved thinking format: Auto is Qwen's when the chat template has the enable_thinking switch):
// as LlamaCppBackend, the backend writes the thinking block's texts itself. A call that thinks is two /completion
// calls on one slot: the block, which stops at the format's end marker or at its budget, then the answer, whose prompt
// extends the first one's, so the server reuses its cache.
//
// Not thread-safe: one thread owns it. Several backends may talk to one server at once, each on its own slot.
class LlamaServerBackend final : public inference::LlmBackend {
private:
    // The thinking block's texts as the chat server tokenizes them, as LlamaCppBackend::ThinkingMarkers.
    struct ThinkingMarkers {
        std::vector<std::int32_t> open = {};     // the format's open text, special tokens parsed.
        std::vector<std::int32_t> skip = {};     // the format's skip text, special tokens parsed.
        std::vector<std::int32_t> close = {};    // the format's close text, special tokens parsed.
        std::vector<std::int32_t> end = {};      // the format's end text, special tokens parsed.
        std::vector<std::int32_t> afterEnd = {}; // the format's afterEnd text, as plain text.
        std::string openText = {};               // The open text, cut off a rendered prompt that ends with it.
        bool templateWritesOpen = false; // The chat template has the switch: only then may it write the open text.
        std::string endText = {};                // The end marker's text: the thinking request's stop string.
    };

    LlamaServerConfig _config = {};
    std::unique_ptr<httplib::Client> _pChatClient = nullptr;      // Kept alive to the chat server, for every request
                                                                  // but /completion.
    std::unique_ptr<httplib::Client> _pStreamClient = nullptr;    // Kept alive to the chat server, for /completion only.
    std::unique_ptr<httplib::Client> _pEmbeddingClient = nullptr; // Kept alive to the embedding server.
    std::optional<ThinkingMarkers> _thinking = {}; // Set when a thinking format resolved.
    std::optional<std::int32_t> _bos = {};         // The BOS token the chat model adds at a prompt's start, if any.
    std::string _embeddingModelId = {};            // The embedding server's model file name, without its extension.
    int _slotCount = 0;                            // The chat server's slots (/props total_slots).
    std::string _chatModelId = {};                 // The chat server's model file name, without its extension.

public:
    // Makes the kept-alive clients (the chat server's two, the embedding server's one), then reads both servers' GET
    // /props: the chat server's context per slot, which must hold config.contextSize, its chat template (the
    // thinking setting decides whether the model thinks, the template only under Auto), its slot count and its model
    // file; the embedding server's model file. Then tokenizes the thinking texts (thinking models) and an empty text
    // with add_special (the special tokens the model adds: a BOS in front of a text is kept, an EOS after it ignored).
    // Throws InferenceError naming the URL for a URL that is not http://host or http://host:port (a port from 1 to
    // 65535), an unreachable server, an HTTP error, an unexpected answer, slots smaller than config.contextSize, a chat
    // server with no slot, a thinking format whose close text uses core::MIN_THINKING_TOKENS tokens or more, or a
    // model that adds more than one special token to a text. Throws ContractError for a context size below 1 or a
    // slot below 0.
    explicit LlamaServerBackend(LlamaServerConfig config);
    ~LlamaServerBackend() override;
    LlamaServerBackend(const LlamaServerBackend&) = delete;
    LlamaServerBackend& operator=(const LlamaServerBackend&) = delete;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    // The chat server's slots, from its /props total_slots: what morpheus_server shares between conversations.
    int SlotCount() const;
    // The chat server's model: its /props model_path's file name without the extension, as EmbeddingModelId.
    std::string ChatModelId() const;
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
