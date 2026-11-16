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

namespace morpheus::inference {

// A model or runtime failure: a model file that will not load, a prompt that does not fit, a failed decode.
class InferenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The backend refused this input itself: an HTTP 4xx answer for it, or a text too long for the model's context.
// Sending it again cannot help, unlike a backend that could not be reached or failed (a plain InferenceError). A caller
// that does not tell the two apart catches InferenceError.
class InputRefused : public InferenceError {
public:
    using InferenceError::InferenceError;
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
    float minP = 0.0f; // 0 means no min-p. Last, so positional initializers stay valid.
};

struct GenerationSpec {
    std::string grammar = {}; // GBNF with a "root" rule; empty means unconstrained. Applies to the answer only.
    int maxTokens = 256;      // The answer: the text after any thinking.
    // > 0 lets a thinking model reason before it answers, in a block of at most this many tokens (its markers
    // included). The block is unconstrained, never streamed and kept out of Completion::text. 0: no thinking.
    int thinkingMaxTokens = 0;
    Sampling sampling = {};
    // The server slot whose prompt cache the call uses; -1 is the backend's default slot. The in-process backend has
    // one cache and ignores it; LlamaServerBackend sends it as id_slot. Morpheus's own game leaves it at -1.
    int slot = -1;
    // What the call is, for the layers that tell calls apart (the replay, the app's labels); the backends ignore it.
    // The turn layer sets it on every call (generation-controls spec, section 2).
    std::string callTag = {};
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

// Message contents are text, never control tokens: "<|im_end|>" typed by a player must not end its message. A chat
// template writes each content verbatim, so a backend renders the template with a marker in place of every content
// (MarkContents with ContentMarker), cuts the rendered text at the markers (TemplatePieces), and tokenizes the
// template's pieces with special tokens parsed (its control tokens are real) and the contents as plain text, whatever
// bytes they hold. This assumes the template copies each content verbatim (true for chatml and Qwen3); one that trims
// or rewrites contents is refused only if a marker goes missing or repeats.

// The marker for `prompt`: the first of "\x1E", "\x1E" "1\x1E", "\x1E" "2\x1E", ... that no content contains (ASCII
// record separator, a byte that ordinary text does not use). Each occurrence of a candidate in a content starts at its
// own "\x1E" byte, so the search ends within one more candidate than the contents hold such bytes.
std::string ContentMarker(const Prompt& prompt);

// `prompt` with every message's content replaced by `marker`.
Prompt MarkContents(const Prompt& prompt, std::string_view marker);

// The template's text around the contents of a prompt rendered with `marker` in place of each of its `contentCount`
// contents: contentCount + 1 pieces in order (before the first content, between contents, after the last), any of
// them possibly empty. Throws InferenceError "the chat template repeats a message's content" when the marker appears
// more often, and "the chat template drops a message's content" when it appears less often.
std::vector<std::string> TemplatePieces(std::string_view rendered, std::string_view marker, std::size_t contentCount);

// Counts tokens as the chat model reads them (architecture spec, "Inference interface").
class Tokenizer {
public:
    virtual ~Tokenizer() = default;

    // Tokens in raw text: no chat template, no special tokens.
    virtual int CountTokens(std::string_view text) = 0;

    // Tokens in the full templated prompt, exactly as Complete would evaluate it without thinking. A call with
    // thinking evaluates no more than that: the markers of its thinking block count against thinkingMaxTokens.
    virtual int CountPromptTokens(const Prompt& prompt) = 0;

    virtual int ContextSize() const = 0;
};

// Generates the model's replies, and holds the chat cache they reuse.
class ChatModel {
public:
    virtual ~ChatModel() = default;

    // Generates the assistant's reply to `prompt`: with spec.thinkingMaxTokens > 0 a thinking block first, then the
    // answer, which alone is streamed to onToken and constrained by spec.grammar. Throws InferenceError if prompt +
    // spec.thinkingMaxTokens + spec.maxTokens exceeds the context, or if thinking is asked of a model without a
    // thinking mode. Throws ContractError for maxTokens <= 0, a negative thinkingMaxTokens, one too small to close
    // the block, or (in-process, LlamaCppBackend) a grammar that does not parse; LlamaServerBackend sends the grammar
    // to the server and reports its refusal (HTTP 400) as InferenceError. Cancellation through `stop` returns early
    // with completion.cancelled set; it is not an error.
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;

    // The chat KV cache and the tokens it holds, so the GPU can be handed to another model and the cache restored.
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(std::span<const std::uint8_t> state) = 0;
};

// Turns texts into vectors for retrieval.
class Embedder {
public:
    virtual ~Embedder() = default;

    // One L2-normalized vector per text, in order.
    virtual std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, EmbedPurpose purpose) = 0;

    // Identifies the embedding model, for store::EmbeddingRow::model.
    virtual std::string EmbeddingModelId() const = 0;
};

// The engine's view of its models: all three parts (architecture spec, "Inference interface"). Code that needs one
// part can take that part alone. Whether a text fits a grammar is grammar::Check's job, independent of any backend.
class LlmBackend : public Tokenizer, public ChatModel, public Embedder {};

} // namespace morpheus::inference
