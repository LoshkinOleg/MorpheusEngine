#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/Settings.h"

namespace morpheus::inference {

// A model or runtime failure: a model file that will not load, a prompt that does not fit, a failed decode.
class InferenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Role { System, User, Assistant };

// Role spelling used by chat templates: "system", "user", "assistant".
std::string RoleName(Role role);

struct Message {
    Role role = Role::User;
    std::string content = {};
};

// A chat prompt. messages[0, stableCount) is the stable prefix (layers L0-L3); the rest is the volatile tail.
// The llama.cpp backend reuses the KV cache by longest common token prefix, so stableCount is informational for
// it; a cloud backend places its prompt-cache breakpoint there.
struct Prompt {
    std::vector<Message> messages = {};
    std::size_t stableCount = 0;
};

struct Sampling {
    float temperature = 0.0f; // <= 0 means greedy decoding.
    int topK = 40;
    float topP = 0.95f;
    std::uint32_t seed = 0;
};

// The sampling for one call from its settings.json values and the call's seed (core::SamplingSeed).
Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);

struct GenerationSpec {
    std::string grammar = {}; // GBNF with a "root" rule; empty means unconstrained. Applies to the answer only.
    int maxTokens = 256;      // The answer: the text after any thinking.
    // > 0 lets a thinking model reason before it answers, in a block of at most this many tokens (its markers
    // included). The block is unconstrained, never streamed and kept out of Completion::text. 0: no thinking.
    int thinkingMaxTokens = 0;
    Sampling sampling = {};
};

struct Completion {
    std::string text = {};     // The answer. Empty when the model ended its reply inside the thinking block.
    std::string thinking = {}; // The reasoning written in the thinking block, without its markers.
    int thinkingTokens = 0;    // Tokens the thinking block took, markers included.
    bool thinkingStoppedAtLimit = false; // The block reached thinkingMaxTokens and the backend closed it.
    double thinkingMs = 0.0;
    int promptTokens = 0;     // Tokens in the templated prompt.
    int tokensReused = 0;     // Prompt tokens served from the KV cache.
    int tokensEvaluated = 0;  // Prompt tokens computed for this call.
    int tokensGenerated = 0;     // Answer tokens.
    bool stoppedAtLimit = false; // The answer reached maxTokens before an end-of-generation token.
    bool cancelled = false;
    double promptMs = 0.0;
    double generateMs = 0.0; // The answer; thinkingMs covers the thinking block.
};

enum class EmbedPurpose { Query, Document };

// Receives generated text as it streams. Pieces never split a UTF-8 character.
using TokenCallback = std::function<void(std::string_view piece)>;

// The engine's only view of a language model (architecture spec, "Inference interface").
class LlmBackend {
public:
    virtual ~LlmBackend() = default;

    // Tokens in raw text: no chat template, no special tokens.
    virtual int CountTokens(std::string_view text) = 0;

    // Tokens in the full templated prompt, exactly as Complete would evaluate it without thinking. A call with
    // thinking evaluates no more than that: the markers of its thinking block count against thinkingMaxTokens.
    virtual int CountPromptTokens(const Prompt& prompt) = 0;

    virtual int ContextSize() const = 0;

    // Generates the assistant's reply to `prompt`: with spec.thinkingMaxTokens > 0 a thinking block first, then the
    // answer, which alone is streamed to onToken and constrained by spec.grammar. Throws InferenceError if prompt +
    // spec.thinkingMaxTokens + spec.maxTokens exceeds the context, or if thinking is asked of a model without a
    // thinking mode. Throws ContractError for maxTokens <= 0, a negative thinkingMaxTokens, one too small to close
    // the block, or a grammar that does not parse. Cancellation through `stop` returns early with
    // completion.cancelled set; it is not an error.
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;

    // One L2-normalized vector per text, in order.
    virtual std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, EmbedPurpose purpose) = 0;

    // Identifies the embedding model, for store::EmbeddingRow::model.
    virtual std::string EmbeddingModelId() const = 0;

    // The chat KV cache and the tokens it holds, so the GPU can be handed to another model and the cache restored.
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(std::span<const std::uint8_t> state) = 0;
};

} // namespace morpheus::inference
