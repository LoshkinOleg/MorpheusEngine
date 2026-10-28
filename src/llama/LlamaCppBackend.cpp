#include "llama/LlamaCppBackend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <mutex>
#include <string_view>

#include <llama.h>

#include "core/Errors.h"
#include "core/Text.h"

namespace morpheus::llama {

using inference::InferenceError;

namespace {

// Buffer for one token piece (llama_token_to_piece); a longer piece is an error.
constexpr std::size_t TOKEN_PIECE_BUFFER_BYTES = 256;

// Stands in for message contents while the chat template renders (TokenizePrompt). The turn layer strips control
// characters from player input, so a real message never contains it.
constexpr char CONTENT_MARKER = ''; // ASCII record separator.

// The log sink and the level of the line being logged, shared by every thread llama.cpp logs from.
struct LogRoute {
    std::mutex mutex = {};
    LogSink sink = {};
    LogLevel lastLevel = LogLevel::Info; // Continuation pieces (GGML_LOG_LEVEL_CONT) inherit it.
};

LogRoute& Route()
{
    static LogRoute route;
    return route;
}

// llama.cpp and ggml log through one process-wide callback; it forwards to the sink.
void LogCallback(ggml_log_level level, const char* pText, void* /*pUserData*/)
{
    LogRoute& route = Route();
    std::scoped_lock lock(route.mutex);
    switch (level) {
    case GGML_LOG_LEVEL_DEBUG:
        route.lastLevel = LogLevel::Debug;
        break;
    case GGML_LOG_LEVEL_WARN:
        route.lastLevel = LogLevel::Warn;
        break;
    case GGML_LOG_LEVEL_ERROR:
        route.lastLevel = LogLevel::Error;
        break;
    case GGML_LOG_LEVEL_CONT:
        break; // Continues the previous line at its level.
    default:
        route.lastLevel = LogLevel::Info; // INFO and NONE.
        break;
    }
    const std::string_view text = pText != nullptr ? pText : "";
    if (route.sink) {
        route.sink(route.lastLevel, text);
    } else if (route.lastLevel == LogLevel::Warn || route.lastLevel == LogLevel::Error) {
        std::fwrite(text.data(), 1, text.size(), stderr);
    }
}

// Owns a llama_batch from llama_batch_init.
struct BatchGuard {
    llama_batch batch; // Set by the constructor (llama_batch_init), so no default initializer.

    explicit BatchGuard(std::size_t capacity)
        : batch(llama_batch_init(static_cast<int32_t>(capacity), 0, 1))
    {
    }
    ~BatchGuard()
    {
        llama_batch_free(batch);
    }
    BatchGuard(const BatchGuard&) = delete;
    BatchGuard& operator=(const BatchGuard&) = delete;
};

enum class Outputs { Last, All, None };

// Fills a batch with `tokens` for sequence 0, at positions startPosition onward.
void FillBatch(llama_batch& batch, std::span<const std::int32_t> tokens, std::size_t startPosition, Outputs outputs)
{
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = static_cast<llama_pos>(startPosition + i);
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        const bool output = outputs == Outputs::All || (outputs == Outputs::Last && i + 1 == tokens.size());
        batch.logits[i] = static_cast<int8_t>(output ? 1 : 0);
    }
    batch.n_tokens = static_cast<int32_t>(tokens.size());
}

struct SamplerDeleter {
    void operator()(llama_sampler* pSampler) const
    {
        llama_sampler_free(pSampler);
    }
};

} // namespace

void SetLogSink(LogSink sink)
{
    LogRoute& route = Route();
    std::scoped_lock lock(route.mutex);
    route.sink = std::move(sink);
}

void LlamaCppBackend::ModelDeleter::operator()(llama_model* pModel) const
{
    llama_model_free(pModel);
}

void LlamaCppBackend::ContextDeleter::operator()(llama_context* pContext) const
{
    llama_free(pContext);
}

LlamaCppBackend::LlamaCppBackend(LlamaConfig config)
    : _config(std::move(config))
{
    MORPHEUS_REQUIRE(_config.contextSize > 0 && _config.embeddingContextSize > 0,
                     "LlamaCppBackend: context sizes must be positive");

    // llama.cpp's global state is initialised once per process and never torn down.
    static std::once_flag initOnce;
    std::call_once(initOnce, [] {
        llama_log_set(LogCallback, nullptr);
        llama_backend_init();
    });

    auto loadModel = [this](const std::filesystem::path& path) {
        llama_model_params params = llama_model_default_params();
        params.n_gpu_layers = _config.gpuLayers;
        // ggml opens model files as UTF-8 on Windows, so pass UTF-8 bytes, not the ANSI path.string().
        const std::u8string pathUtf8 = path.u8string();
        llama_model* pModel = llama_model_load_from_file(reinterpret_cast<const char*>(pathUtf8.c_str()), params);
        if (pModel == nullptr) {
            throw InferenceError(std::format("cannot load model '{}'", core::PathText(path)));
        }
        return std::unique_ptr<llama_model, ModelDeleter>(pModel);
    };

    // Chat model: generation and token counting on sequence 0.
    _pChatModel = loadModel(_config.chatModel);
    {
        llama_context_params params = llama_context_default_params();
        params.n_ctx = static_cast<uint32_t>(_config.contextSize);
        params.n_seq_max = 1;
        _pChatContext.reset(llama_init_from_model(_pChatModel.get(), params));
        if (!_pChatContext) {
            throw InferenceError(std::format("cannot create a chat context of {} tokens", _config.contextSize));
        }
    }

    // Embedding model: pooled sentence embeddings. A whole text is one ubatch, as encoder models require.
    _pEmbedModel = loadModel(_config.embeddingModel);
    {
        llama_context_params params = llama_context_default_params();
        const auto size = static_cast<uint32_t>(_config.embeddingContextSize);
        params.n_ctx = size;
        params.n_batch = size;
        params.n_ubatch = size;
        params.n_seq_max = 1;
        params.embeddings = true;
        _pEmbedContext.reset(llama_init_from_model(_pEmbedModel.get(), params));
        if (!_pEmbedContext) {
            throw InferenceError("cannot create the embedding context");
        }
        if (llama_pooling_type(_pEmbedContext.get()) == LLAMA_POOLING_TYPE_NONE) {
            throw InferenceError(std::format("'{}' has no pooling; use a sentence-embedding model",
                                             core::PathText(_config.embeddingModel)));
        }
        if (llama_pooling_type(_pEmbedContext.get()) == LLAMA_POOLING_TYPE_RANK) {
            throw InferenceError(std::format("'{}' is a reranker, not a sentence-embedding model",
                                             core::PathText(_config.embeddingModel)));
        }
    }
}

LlamaCppBackend::~LlamaCppBackend() = default;

int LlamaCppBackend::CountTokens(std::string_view text)
{
    return static_cast<int>(Tokenize(llama_model_get_vocab(_pChatModel.get()), text, false, false).size());
}

int LlamaCppBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return static_cast<int>(TokenizePrompt(prompt).size());
}

int LlamaCppBackend::ContextSize() const
{
    return static_cast<int>(llama_n_ctx(_pChatContext.get()));
}

inference::Completion LlamaCppBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                const inference::TokenCallback& onToken, std::stop_token stop)
{
    MORPHEUS_REQUIRE(spec.maxTokens > 0, "Complete: maxTokens must be positive");
    using Clock = std::chrono::steady_clock;
    const Clock::time_point promptStart = Clock::now();
    const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());

    inference::Completion completion;
    const std::vector<std::int32_t> tokens = TokenizePrompt(prompt);
    completion.promptTokens = static_cast<int>(tokens.size());
    if (spec.maxTokens > ContextSize() - completion.promptTokens) {
        throw InferenceError(std::format("prompt ({} tokens) plus max_tokens ({}) exceeds the context size ({})",
                                         completion.promptTokens, spec.maxTokens, ContextSize()));
    }

    // Sampler chain: the grammar first (it masks impossible tokens), then the sampling settings. Built before the
    // prompt is decoded, so an invalid grammar fails without touching the KV cache.
    std::unique_ptr<llama_sampler, SamplerDeleter> pSampler(llama_sampler_chain_init(llama_sampler_chain_default_params()));
    if (!spec.grammar.empty()) {
        llama_sampler* pGrammar = llama_sampler_init_grammar(pVocab, spec.grammar.c_str(), "root");
        MORPHEUS_REQUIRE(pGrammar != nullptr, "Complete: the GBNF grammar does not parse");
        llama_sampler_chain_add(pSampler.get(), pGrammar);
    }
    if (spec.sampling.temperature <= 0.0f) {
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_top_k(spec.sampling.topK));
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_top_p(spec.sampling.topP, 1));
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_temp(spec.sampling.temperature));
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_dist(spec.sampling.seed));
    }

    // Reuse the longest common prefix of the KV cache. The last prompt token is always evaluated, because
    // sampling needs its logits.
    std::size_t common = 0;
    while (common < _cachedTokens.size() && common < tokens.size() && _cachedTokens[common] == tokens[common]) {
        ++common;
    }
    if (common == tokens.size()) {
        --common;
    }
    llama_memory_t pMemory = llama_get_memory(_pChatContext.get());
    if (!llama_memory_seq_rm(pMemory, 0, static_cast<llama_pos>(common), -1)) {
        // Some model types cannot drop part of a sequence; start from an empty cache instead.
        llama_memory_clear(pMemory, true);
        common = 0;
    }
    _cachedTokens.resize(common);
    completion.tokensReused = static_cast<int>(common);

    // Evaluate the rest of the prompt in n_batch chunks; only the final chunk needs logits.
    const std::size_t batchSize = llama_n_batch(_pChatContext.get());
    for (std::size_t position = common; position < tokens.size(); position += batchSize) {
        if (stop.stop_requested()) {
            completion.cancelled = true;
            completion.tokensEvaluated = static_cast<int>(_cachedTokens.size() - common);
            completion.promptMs = std::chrono::duration<double, std::milli>(Clock::now() - promptStart).count();
            return completion;
        }
        const std::size_t count = std::min(batchSize, tokens.size() - position);
        DecodeChat(std::span(tokens).subspan(position, count), position + count == tokens.size());
    }
    completion.tokensEvaluated = static_cast<int>(_cachedTokens.size() - common);
    const Clock::time_point generateStart = Clock::now();
    completion.promptMs = std::chrono::duration<double, std::milli>(generateStart - promptStart).count();

    // Length of the longest prefix of `text` that does not end inside a UTF-8 sequence, so streamed pieces never
    // split a character.
    auto completeUtf8Prefix = [](std::string_view text) -> std::size_t {
        for (std::size_t back = 1; back <= 4 && back <= text.size(); ++back) {
            const auto byte = static_cast<unsigned char>(text[text.size() - back]);
            if ((byte & 0xC0) == 0x80) {
                continue; // Continuation byte: keep looking for the lead byte.
            }
            const std::size_t length = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xE ? 3 : (byte >> 3) == 0x1E ? 4 : 1;
            return length <= back ? text.size() : text.size() - back;
        }
        return text.size(); // Only continuation bytes: malformed, emit as is.
    };

    // Generate. Every accepted token is decoded right away, so the cache always matches _cachedTokens.
    std::string pending; // Generated bytes not yet streamed (an incomplete UTF-8 character).
    for (int i = 0; i < spec.maxTokens; ++i) {
        if (stop.stop_requested()) {
            completion.cancelled = true;
            break;
        }
        const llama_token token = llama_sampler_sample(pSampler.get(), _pChatContext.get(), -1);
        if (llama_vocab_is_eog(pVocab, token)) {
            break;
        }

        char piece[TOKEN_PIECE_BUFFER_BYTES];
        const int32_t length = llama_token_to_piece(pVocab, token, piece, static_cast<int32_t>(sizeof(piece)), 0, false);
        MORPHEUS_REQUIRE(length >= 0, std::format("Complete: a token piece is longer than {} bytes", TOKEN_PIECE_BUFFER_BYTES));
        completion.text.append(piece, static_cast<std::size_t>(length));
        pending.append(piece, static_cast<std::size_t>(length));
        const std::size_t ready = completeUtf8Prefix(pending);
        if (ready > 0 && onToken) {
            onToken(std::string_view(pending).substr(0, ready));
        }
        pending.erase(0, ready);

        ++completion.tokensGenerated;
        const std::int32_t generated = token;
        DecodeChat(std::span(&generated, 1), true);
        if (i + 1 == spec.maxTokens) {
            completion.stoppedAtLimit = true;
        }
    }
    if (!pending.empty() && onToken) {
        onToken(pending);
    }
    completion.generateMs = std::chrono::duration<double, std::milli>(Clock::now() - generateStart).count();
    return completion;
}

std::vector<std::vector<float>> LlamaCppBackend::Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose)
{
    const llama_vocab* pVocab = llama_model_get_vocab(_pEmbedModel.get());
    const std::string& prefix = purpose == inference::EmbedPurpose::Query ? _config.embeddingQueryPrefix
                                                                          : _config.embeddingDocumentPrefix;
    const auto dimensions = static_cast<std::size_t>(llama_model_n_embd_out(_pEmbedModel.get()));
    const bool encoderOnly = llama_model_has_encoder(_pEmbedModel.get()) && !llama_model_has_decoder(_pEmbedModel.get());

    std::vector<std::vector<float>> vectors;
    vectors.reserve(texts.size());
    for (const std::string& text : texts) {
        const std::vector<std::int32_t> tokens = Tokenize(pVocab, prefix + text, true, false);
        if (tokens.empty() || tokens.size() > static_cast<std::size_t>(_config.embeddingContextSize)) {
            throw InferenceError(std::format("a text to embed has {} tokens; the embedding context holds {}",
                                             tokens.size(), _config.embeddingContextSize));
        }

        // Each text is embedded alone, from an empty cache (encoder-only models have no cache at all).
        if (llama_memory_t pMemory = llama_get_memory(_pEmbedContext.get()); pMemory != nullptr) {
            llama_memory_clear(pMemory, true);
        }
        BatchGuard guard(tokens.size());
        FillBatch(guard.batch, tokens, 0, Outputs::All);
        const int32_t result = encoderOnly ? llama_encode(_pEmbedContext.get(), guard.batch)
                                           : llama_decode(_pEmbedContext.get(), guard.batch);
        if (result != 0) {
            throw InferenceError(std::format("embedding failed with code {}", result));
        }

        const float* pEmbedding = llama_get_embeddings_seq(_pEmbedContext.get(), 0);
        if (pEmbedding == nullptr) {
            throw InferenceError("the embedding model returned no pooled embedding");
        }
        std::vector<float> vector(pEmbedding, pEmbedding + dimensions);

        // L2-normalize, so cosine similarity is a dot product.
        double norm = 0.0;
        for (const float value : vector) {
            norm += static_cast<double>(value) * value;
        }
        norm = std::sqrt(norm);
        if (norm == 0.0) {
            throw InferenceError("the embedding model returned a zero vector");
        }
        for (float& value : vector) {
            value = static_cast<float>(value / norm);
        }
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

std::string LlamaCppBackend::EmbeddingModelId() const
{
    // UTF-8, so the id is the same on every machine and code page.
    return core::PathText(_config.embeddingModel.stem());
}

std::vector<std::uint8_t> LlamaCppBackend::SaveState()
{
    // Layout: [token count: uint64][tokens: int32 each][llama.cpp sequence state].
    const std::size_t stateSize = llama_state_seq_get_size(_pChatContext.get(), 0);
    const std::uint64_t count = _cachedTokens.size();
    const std::size_t tokenBytes = _cachedTokens.size() * sizeof(std::int32_t);
    std::vector<std::uint8_t> blob(sizeof(count) + tokenBytes + stateSize);
    std::memcpy(blob.data(), &count, sizeof(count));
    if (tokenBytes > 0) {
        std::memcpy(blob.data() + sizeof(count), _cachedTokens.data(), tokenBytes);
    }
    const std::size_t written = llama_state_seq_get_data(_pChatContext.get(), blob.data() + sizeof(count) + tokenBytes, stateSize, 0);
    if (written != stateSize) {
        throw InferenceError("saving the KV cache state failed");
    }
    return blob;
}

void LlamaCppBackend::RestoreState(std::span<const std::uint8_t> state)
{
    // The blob comes from SaveState, so a malformed one is an engine bug.
    std::uint64_t count = 0;
    MORPHEUS_REQUIRE(state.size() >= sizeof(count), "RestoreState: state blob too small");
    std::memcpy(&count, state.data(), sizeof(count));
    MORPHEUS_REQUIRE(count <= (state.size() - sizeof(count)) / sizeof(std::int32_t), "RestoreState: state blob truncated");
    const std::size_t tokenBytes = static_cast<std::size_t>(count) * sizeof(std::int32_t);
    std::vector<std::int32_t> tokens(static_cast<std::size_t>(count));
    if (tokenBytes > 0) {
        std::memcpy(tokens.data(), state.data() + sizeof(count), tokenBytes);
    }

    llama_memory_t pMemory = llama_get_memory(_pChatContext.get());
    llama_memory_clear(pMemory, true);
    _cachedTokens.clear();
    const std::size_t kvSize = state.size() - sizeof(count) - tokenBytes;
    // State blobs are in-memory only (a GPU hand-off), never persisted. A malformed header above is an engine bug
    // (ContractError); a state llama.cpp rejects, or one that disagrees with its token list, is a runtime failure
    // (InferenceError). Either failure below can leave a partly restored cache, so empty it: the cache and
    // _cachedTokens (empty) must always agree.
    if (llama_state_seq_set_data(_pChatContext.get(), state.data() + sizeof(count) + tokenBytes, kvSize, 0) == 0) {
        llama_memory_clear(pMemory, true);
        throw InferenceError("restoring the KV cache state failed");
    }
    if (llama_memory_seq_pos_max(pMemory, 0) + 1 != static_cast<llama_pos>(count)) {
        llama_memory_clear(pMemory, true);
        throw InferenceError("restored KV state does not match its token list");
    }
    _cachedTokens = std::move(tokens);
}

std::string LlamaCppBackend::RenderPrompt(const inference::Prompt& prompt) const
{
    MORPHEUS_REQUIRE(!prompt.messages.empty(), "RenderPrompt: the prompt has no messages");
    const char* pTemplate = llama_model_chat_template(_pChatModel.get(), nullptr);
    if (pTemplate == nullptr) {
        throw InferenceError("the chat model has no built-in chat template");
    }

    // llama_chat_message holds raw pointers: keep the role strings alive (reserve: no reallocation).
    std::vector<std::string> roles;
    roles.reserve(prompt.messages.size());
    std::vector<llama_chat_message> chat;
    chat.reserve(prompt.messages.size());
    std::size_t contentBytes = 0;
    for (const inference::Message& message : prompt.messages) {
        roles.push_back(inference::RoleName(message.role));
        chat.push_back({roles.back().c_str(), message.content.c_str()});
        contentBytes += message.content.size();
    }

    // add_ass: the rendered prompt ends where the assistant's reply begins. Retry once if the buffer was short.
    std::vector<char> buffer(2 * contentBytes + 1024);
    int32_t length = llama_chat_apply_template(pTemplate, chat.data(), chat.size(), true, buffer.data(),
                                               static_cast<int32_t>(buffer.size()));
    if (length < 0) {
        throw InferenceError("llama_chat_apply_template does not support this model's chat template");
    }
    if (static_cast<std::size_t>(length) > buffer.size()) {
        buffer.resize(static_cast<std::size_t>(length));
        length = llama_chat_apply_template(pTemplate, chat.data(), chat.size(), true, buffer.data(),
                                           static_cast<int32_t>(buffer.size()));
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

std::vector<std::int32_t> LlamaCppBackend::TokenizePrompt(const inference::Prompt& prompt) const
{
    // Message contents are text, never control tokens: "<|im_end|>" typed by a player must not end its message. So
    // the template is rendered with a marker in place of each content, and the pieces are tokenized apart:
    // template text with parse_special (its control tokens are real), contents without it.
    inference::Prompt marked = prompt;
    for (inference::Message& message : marked.messages) {
        if (message.content.find(CONTENT_MARKER) != std::string::npos) {
            throw InferenceError("a message contains the reserved character U+001E");
        }
        message.content = std::string(1, CONTENT_MARKER);
    }
    const std::string rendered = RenderPrompt(marked);

    const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());
    // BOS is added when the model expects one; if the template already wrote it, the duplicate is dropped below.
    const bool addBos = llama_vocab_get_add_bos(pVocab);
    std::vector<std::int32_t> tokens;
    auto append = [&tokens](const std::vector<std::int32_t>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
    std::size_t start = 0;
    std::size_t message = 0;
    for (;;) {
        const std::size_t marker = rendered.find(CONTENT_MARKER, start);
        const std::string_view templateText =
            std::string_view(rendered).substr(start, marker == std::string::npos ? std::string::npos : marker - start);
        if (!templateText.empty() || (start == 0 && addBos)) {
            append(Tokenize(pVocab, templateText, start == 0 && addBos, true));
        }
        if (marker == std::string::npos) {
            break;
        }
        if (message == prompt.messages.size()) {
            throw InferenceError("the chat template repeats a message's content");
        }
        if (!prompt.messages[message].content.empty()) {
            append(Tokenize(pVocab, prompt.messages[message].content, false, false));
        }
        ++message;
        start = marker + 1;
    }
    if (message != prompt.messages.size()) {
        throw InferenceError("the chat template drops a message's content");
    }
    if (addBos && tokens.size() >= 2 && tokens[0] == llama_vocab_bos(pVocab) && tokens[1] == tokens[0]) {
        tokens.erase(tokens.begin());
    }
    MORPHEUS_REQUIRE(!tokens.empty(), "TokenizePrompt: the rendered prompt has no tokens");
    return tokens;
}

void LlamaCppBackend::DecodeChat(std::span<const std::int32_t> tokens, bool wantLogits)
{
    BatchGuard guard(tokens.size());
    FillBatch(guard.batch, tokens, _cachedTokens.size(), wantLogits ? Outputs::Last : Outputs::None);
    const int32_t result = llama_decode(_pChatContext.get(), guard.batch);
    if (result != 0) {
        // A failed decode can leave part of the batch in the cache: start from an empty cache next time.
        llama_memory_clear(llama_get_memory(_pChatContext.get()), true);
        _cachedTokens.clear();
        throw InferenceError(std::format("llama_decode failed with code {}", result));
    }
    _cachedTokens.insert(_cachedTokens.end(), tokens.begin(), tokens.end());
}

std::vector<std::int32_t> LlamaCppBackend::Tokenize(const llama_vocab* pVocab, std::string_view text, bool addSpecial,
                                                    bool parseSpecial)
{
    MORPHEUS_REQUIRE(text.size() <= static_cast<std::size_t>(std::numeric_limits<int32_t>::max()),
                     "Tokenize: text is too long");
    const auto length = static_cast<int32_t>(text.size());
    // The first call measures: it returns minus the number of tokens needed.
    const int32_t needed = llama_tokenize(pVocab, text.data(), length, nullptr, 0, addSpecial, parseSpecial);
    if (needed == std::numeric_limits<int32_t>::min()) {
        throw InferenceError("tokenization overflowed");
    }
    std::vector<std::int32_t> tokens(static_cast<std::size_t>(needed < 0 ? -needed : needed));
    const int32_t written = llama_tokenize(pVocab, text.data(), length, tokens.data(), static_cast<int32_t>(tokens.size()),
                                           addSpecial, parseSpecial);
    if (written < 0) {
        throw InferenceError("tokenization failed");
    }
    tokens.resize(static_cast<std::size_t>(written));
    return tokens;
}

} // namespace morpheus::llama
