### Task 5: LlamaCppBackend

**Files:**
- Create: `src/llama/CMakeLists.txt`, `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp`, `tests/llama/LlamaTestModels.h`, `tests/llama/LlamaCompletionTests.cpp`
- Modify: root `CMakeLists.txt` (`add_subdirectory(src/llama)` inside `if(MORPHEUS_BUILD_LLAMA)`), `tests/CMakeLists.txt` (link `morpheus_llama` when `MORPHEUS_BUILD_LLAMA`)

**Interfaces:**
- Consumes: everything in `inference/Llm.h` (Task 4).
- Produces (namespace `morpheus::llama`):
  - `struct LlamaConfig { std::filesystem::path chatModel, embeddingModel; std::string embeddingQueryPrefix, embeddingDocumentPrefix; int contextSize = 8192; int gpuLayers = -1; int embeddingContextSize = 2048; }`
  - `class LlamaCppBackend final : public inference::LlmBackend`, with `explicit LlamaCppBackend(LlamaConfig)`
- llama.cpp facts used, checked against `build/dev/_deps/llama-src/include/llama.h`:
  - `llama_memory_seq_rm(mem, seq, p0, p1)` returns false when a partial removal is unsupported.
  - `llama_chat_apply_template(tmpl, chat, n, add_ass, buf, len)` returns the needed length.
  - `llama_sampler_init_grammar(vocab, gbnf, "root")` returns NULL on a parse error.
  - `llama_sampler_sample` also accepts the token into the chain.
  - `llama_state_seq_get_size/get_data/set_data` (set_data returns 0 on failure).
  - `llama_get_embeddings_seq` returns NULL without pooling.

- [ ] **Step 1: Write the shared test fixture `tests/llama/LlamaTestModels.h`**

```cpp
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <string>

#include "llama/LlamaCppBackend.h"

namespace morpheus::test {

// Reads an environment variable; "" when unset. _dupenv_s avoids MSVC's getenv deprecation warning under /WX.
inline std::string EnvironmentVariable(const char* name)
{
#ifdef _MSC_VER
    char* pValue = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&pValue, &length, name) != 0 || pValue == nullptr) {
        return {};
    }
    std::string value(pValue);
    std::free(pValue);
    return value;
#else
    const char* pValue = std::getenv(name);
    return pValue != nullptr ? pValue : "";
#endif
}

// Model-backed tests need real GGUF files, named by environment variables:
//   MORPHEUS_TEST_CHAT_MODEL   chat model (Qwen3-4B-Instruct-2507-Q4_K_M.gguf)
//   MORPHEUS_TEST_EMBED_MODEL  embedding model (nomic-embed-text-v1.5.Q8_0.gguf)
// The calling test is skipped when either is unset. Loading is slow, so every test shares one backend.
inline llama::LlamaCppBackend& SharedBackend()
{
    const std::string chatModel = EnvironmentVariable("MORPHEUS_TEST_CHAT_MODEL");
    const std::string embedModel = EnvironmentVariable("MORPHEUS_TEST_EMBED_MODEL");
    if (chatModel.empty() || embedModel.empty()) {
        SKIP("set MORPHEUS_TEST_CHAT_MODEL and MORPHEUS_TEST_EMBED_MODEL to run model-backed tests");
    }
    static std::unique_ptr<llama::LlamaCppBackend> pBackend = [&] {
        llama::LlamaConfig config;
        config.chatModel = chatModel;
        config.embeddingModel = embedModel;
        config.embeddingQueryPrefix = "search_query: ";
        config.embeddingDocumentPrefix = "search_document: ";
        config.contextSize = 4096;
        config.gpuLayers = -1;
        return std::make_unique<llama::LlamaCppBackend>(config);
    }();
    return *pBackend;
}

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing test `tests/llama/LlamaCompletionTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <string>

#include "core/Errors.h"
#include "llama/LlamaTestModels.h"

using namespace morpheus::inference;
using morpheus::test::SharedBackend;

namespace {

Prompt SimplePrompt(const std::string& question)
{
    Prompt prompt;
    prompt.messages = {{Role::System, "You are a terse assistant."}, {Role::User, question}};
    prompt.stableCount = 1;
    return prompt;
}

// Greedy decoding (temperature 0) keeps model-backed tests as repeatable as the hardware allows.
GenerationSpec Greedy(int maxTokens)
{
    GenerationSpec spec;
    spec.maxTokens = maxTokens;
    return spec;
}

} // namespace

TEST_CASE("LlamaCppBackend counts tokens", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    CHECK(backend.CountTokens("") == 0);
    const int sentence = backend.CountTokens("The crawler grinds across the dunes.");
    CHECK(sentence > 3);
    CHECK(sentence < 20);
    CHECK(backend.CountPromptTokens(SimplePrompt("Hi")) > backend.CountTokens("Hi"));
    CHECK(backend.ContextSize() == 4096);
}

TEST_CASE("LlamaCppBackend generates and streams text", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    std::string streamed;
    const Completion completion = backend.Complete(SimplePrompt("Say one short sentence about sand."), Greedy(24),
                                                   [&](std::string_view piece) { streamed += piece; }, {});
    CHECK_FALSE(completion.text.empty());
    CHECK(streamed == completion.text);
    CHECK(completion.tokensGenerated > 0);
    CHECK(completion.tokensGenerated <= 24);
    CHECK_FALSE(completion.cancelled);
}

TEST_CASE("LlamaCppBackend reuses the cached prompt prefix", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const Prompt first = SimplePrompt("Name a color.");
    const Completion a = backend.Complete(first, Greedy(8), {}, {});

    // The same prompt again: everything but the last prompt token comes from the cache.
    const Completion b = backend.Complete(first, Greedy(8), {}, {});
    CHECK(b.tokensReused == b.promptTokens - 1);
    CHECK(b.tokensEvaluated == 1);

    // Appending messages (the narrate-call shape) keeps the whole earlier prompt cached.
    Prompt longer = first;
    longer.messages.push_back({Role::Assistant, a.text});
    longer.messages.push_back({Role::User, "Name another."});
    const Completion c = backend.Complete(longer, Greedy(8), {}, {});
    CHECK(c.tokensReused >= a.promptTokens);
}

TEST_CASE("LlamaCppBackend follows a GBNF grammar", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec = Greedy(4);
    spec.grammar = R"(root ::= "yes" | "no")";
    const Completion completion = backend.Complete(SimplePrompt("Is sand dry? Answer yes or no."), spec, {}, {});
    CHECK((completion.text == "yes" || completion.text == "no"));
}

TEST_CASE("LlamaCppBackend returns early when cancelled", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    std::stop_source source;
    source.request_stop();
    const Completion completion = backend.Complete(SimplePrompt("Tell a long story."), Greedy(64), {}, source.get_token());
    CHECK(completion.cancelled);
    CHECK(completion.tokensGenerated == 0);
}

TEST_CASE("LlamaCppBackend rejects requests that cannot work", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    CHECK_THROWS_AS(backend.Complete(SimplePrompt("Hi"), Greedy(5000), {}, {}), InferenceError);

    GenerationSpec badGrammar = Greedy(4);
    badGrammar.grammar = "root ::= (";
    CHECK_THROWS_AS(backend.Complete(SimplePrompt("Hi"), badGrammar, {}, {}), morpheus::core::ContractError);
}
```

- [ ] **Step 3: Build under `dev`. Expect failure.**

Run: `cmake --build --preset dev`
Expected: FAIL with `error C1083: Cannot open include file: 'llama/LlamaCppBackend.h'`.

- [ ] **Step 4: Create `src/llama/LlamaCppBackend.h`**

```cpp
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
```

- [ ] **Step 5: Create `src/llama/LlamaCppBackend.cpp`**

```cpp
#include "llama/LlamaCppBackend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>

#include <llama.h>

#include "core/Errors.h"

namespace morpheus::llama {

using inference::InferenceError;

namespace {

// llama.cpp and ggml log through one process-wide callback. Only warnings and errors reach stderr.
void LogFilter(ggml_log_level level, const char* pText, void* /*pUserData*/)
{
    if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR) {
        std::fputs(pText, stderr);
    }
}

// Owns a llama_batch from llama_batch_init.
struct BatchGuard {
    llama_batch batch;

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
        llama_log_set(LogFilter, nullptr);
        llama_backend_init();
    });

    auto loadModel = [this](const std::filesystem::path& path) {
        llama_model_params params = llama_model_default_params();
        params.n_gpu_layers = _config.gpuLayers;
        llama_model* pModel = llama_model_load_from_file(path.string().c_str(), params);
        if (pModel == nullptr) {
            throw InferenceError(std::format("cannot load model '{}'", path.generic_string()));
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
                                             _config.embeddingModel.generic_string()));
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
    if (completion.promptTokens + spec.maxTokens > ContextSize()) {
        throw InferenceError(std::format("prompt ({} tokens) plus max_tokens ({}) exceeds the context size ({})",
                                         completion.promptTokens, spec.maxTokens, ContextSize()));
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
    llama_memory_t memory = llama_get_memory(_pChatContext.get());
    if (!llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(common), -1)) {
        // Some model types cannot drop part of a sequence; start from an empty cache instead.
        llama_memory_clear(memory, true);
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
            return completion;
        }
        const std::size_t count = std::min(batchSize, tokens.size() - position);
        DecodeChat(std::span(tokens).subspan(position, count), position + count == tokens.size());
    }
    completion.tokensEvaluated = static_cast<int>(_cachedTokens.size() - common);
    const Clock::time_point generateStart = Clock::now();
    completion.promptMs = std::chrono::duration<double, std::milli>(generateStart - promptStart).count();

    // Sampler chain: the grammar first (it masks impossible tokens), then the sampling settings.
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

        char piece[256];
        const int32_t length = llama_token_to_piece(pVocab, token, piece, static_cast<int32_t>(sizeof(piece)), 0, false);
        MORPHEUS_REQUIRE(length >= 0, "Complete: a token piece is longer than 256 bytes");
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
    const auto dimensions = static_cast<std::size_t>(llama_model_n_embd(_pEmbedModel.get()));
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
        if (llama_memory_t memory = llama_get_memory(_pEmbedContext.get()); memory != nullptr) {
            llama_memory_clear(memory, true);
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
    return _config.embeddingModel.stem().string();
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
    const std::size_t tokenBytes = static_cast<std::size_t>(count) * sizeof(std::int32_t);
    MORPHEUS_REQUIRE(state.size() >= sizeof(count) + tokenBytes, "RestoreState: state blob truncated");
    std::vector<std::int32_t> tokens(static_cast<std::size_t>(count));
    if (tokenBytes > 0) {
        std::memcpy(tokens.data(), state.data() + sizeof(count), tokenBytes);
    }

    llama_memory_clear(llama_get_memory(_pChatContext.get()), true);
    _cachedTokens.clear();
    const std::size_t kvSize = state.size() - sizeof(count) - tokenBytes;
    if (llama_state_seq_set_data(_pChatContext.get(), state.data() + sizeof(count) + tokenBytes, kvSize, 0) == 0) {
        throw InferenceError("restoring the KV cache state failed");
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
    const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());
    // Special tokens written by the template are real control tokens (parse_special). BOS is added when the
    // model expects one; if the template already wrote it, the duplicate is dropped.
    const bool addBos = llama_vocab_get_add_bos(pVocab);
    std::vector<std::int32_t> tokens = Tokenize(pVocab, RenderPrompt(prompt), addBos, true);
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
```

- [ ] **Step 6: Wire up CMake**

`src/llama/CMakeLists.txt`:

```cmake
add_library(morpheus_llama STATIC
    LlamaCppBackend.cpp
)
target_include_directories(morpheus_llama PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE llama
)
morpheus_target_defaults(morpheus_llama)
```

In the root `CMakeLists.txt`, after `add_subdirectory(src/inference)`, add:

```cmake
if(MORPHEUS_BUILD_LLAMA)
    add_subdirectory(src/llama)
endif()
```

In `tests/CMakeLists.txt`, after `target_link_libraries(morpheus_tests ...)`, add:

```cmake
if(MORPHEUS_BUILD_LLAMA)
    target_link_libraries(morpheus_tests PRIVATE morpheus_llama)
endif()
```

- [ ] **Step 7: Build and run. Expect a pass, or a skip without models.**

Run: `cmake --build --preset dev && ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"`

Expected:
- **Without the env vars:** every `[llama]` test is reported as skipped, and the build has no warnings.
- **With the env vars set** (see "Model setup"): all `[llama]` tests pass.
- If the models are not present on this machine, say so in the report; that is not a failure.

Then run `ctest --preset tests`. Expected: all pass (no llama.cpp in that preset).

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/llama tests
git commit -m "Add the in-process llama.cpp backend" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

