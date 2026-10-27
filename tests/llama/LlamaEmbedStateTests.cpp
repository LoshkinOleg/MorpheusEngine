#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include "core/Errors.h"
#include "llama/LlamaTestModels.h"

using namespace morpheus::inference;
using morpheus::test::SharedBackend;

namespace {

float Dot(const std::vector<float>& a, const std::vector<float>& b)
{
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
}

Prompt SimplePrompt(const std::string& question)
{
    Prompt prompt;
    prompt.messages = {{Role::System, "You are a terse assistant."}, {Role::User, question}};
    prompt.stableCount = 1;
    return prompt;
}

// A normal completion after a rejected restore: the cache must be usable and its accounting consistent.
void CheckCompletionStillWorks(LlmBackend& backend)
{
    GenerationSpec spec;
    spec.maxTokens = 4;
    const Completion completion = backend.Complete(SimplePrompt("Name a desert animal."), spec, {}, {});
    CHECK(completion.tokensReused + completion.tokensEvaluated == completion.promptTokens);
    CHECK(completion.tokensEvaluated >= 1);
}

// After a restore that failed past the cache clear, the cache is empty: even the identical prompt re-evaluates fully.
void CheckCacheWasCleared(LlmBackend& backend)
{
    GenerationSpec spec;
    spec.maxTokens = 4;
    const Completion completion = backend.Complete(SimplePrompt("Name a desert animal."), spec, {}, {});
    CHECK(completion.tokensReused == 0);
    CHECK(completion.tokensEvaluated == completion.promptTokens);
}

} // namespace

TEST_CASE("Embeddings are normalized and rank related texts higher", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const std::vector<std::vector<float>> vectors = backend.Embed(
        {"a tracked crawler crossing the desert", "a vehicle driving over sand dunes", "a recipe for tomato soup"},
        EmbedPurpose::Document);

    REQUIRE(vectors.size() == 3);
    REQUIRE_FALSE(vectors[0].empty());
    for (const std::vector<float>& vector : vectors) {
        CHECK(vector.size() == vectors[0].size());
        CHECK(Dot(vector, vector) == Catch::Approx(1.0f).margin(1e-4));
    }
    CHECK(Dot(vectors[0], vectors[1]) > Dot(vectors[0], vectors[2]));
    CHECK_FALSE(backend.EmbeddingModelId().empty());
}

TEST_CASE("Embedding vectors have the same non-zero size for every text", "[llama]")
{
    // Covers varied text lengths, using EmbedPurpose::Query.
    LlmBackend& backend = SharedBackend();
    const std::vector<std::vector<float>> vectors = backend.Embed(
        {"hi","a considerably longer sentence about a tracked crawler crossing a very wide desert at dusk",
         "water", "one two three four five six seven eight nine ten"},
        EmbedPurpose::Query);

    REQUIRE(vectors.size() == 4);
    REQUIRE(vectors[0].size() > 0);
    for (const std::vector<float>& vector : vectors) {
        CHECK(vector.size() == vectors[0].size());
    }
}

TEST_CASE("Query and document prefixes give different embeddings", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const std::vector<float> query = backend.Embed({"water supplies"}, EmbedPurpose::Query).at(0);
    const std::vector<float> document = backend.Embed({"water supplies"}, EmbedPurpose::Document).at(0);
    CHECK(Dot(query, document) < 0.9999f);
}

TEST_CASE("A saved KV cache restores prefix reuse", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec;
    spec.maxTokens = 4;

    const Prompt original = SimplePrompt("Name a desert animal.");
    backend.Complete(original, spec, {}, {});
    const std::vector<std::uint8_t> saved = backend.SaveState();

    // A different prompt overwrites the cache...
    backend.Complete(SimplePrompt("Count to three."), spec, {}, {});

    // ...and restoring brings the original prefix back.
    backend.RestoreState(saved);
    const Completion again = backend.Complete(original, spec, {}, {});
    CHECK(again.tokensEvaluated == 1);
    CHECK(again.tokensReused == again.promptTokens - 1);
}

TEST_CASE("A truncated state blob is rejected and the backend keeps working", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec;
    spec.maxTokens = 4;
    backend.Complete(SimplePrompt("Name a desert animal."), spec, {}, {});
    const std::vector<std::uint8_t> saved = backend.SaveState();
    REQUIRE(saved.size() > 8);

    // Shorter than the 8-byte token count.
    CHECK_THROWS_AS(backend.RestoreState(std::span<const std::uint8_t>(saved).first(4)), morpheus::core::ContractError);
    CheckCompletionStillWorks(backend);
}

TEST_CASE("A state blob with an absurd token count is rejected and the backend keeps working", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec;
    spec.maxTokens = 4;
    backend.Complete(SimplePrompt("Name a desert animal."), spec, {}, {});
    std::vector<std::uint8_t> corrupt = backend.SaveState();
    REQUIRE(corrupt.size() > 8);

    // Overwrite the leading count with the largest uint64: it must be checked before any allocation.
    const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max();
    std::memcpy(corrupt.data(), &huge, sizeof(huge));
    CHECK_THROWS_AS(backend.RestoreState(corrupt), morpheus::core::ContractError);
    CheckCompletionStillWorks(backend);
}

TEST_CASE("A state blob that llama.cpp rejects is an InferenceError and leaves a cleared cache", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec;
    spec.maxTokens = 4;
    backend.Complete(SimplePrompt("Name a desert animal."), spec, {}, {});
    std::vector<std::uint8_t> corrupt = backend.SaveState();

    // Claim one token fewer than saved: the llama.cpp payload now starts 4 bytes early, so set_data rejects it.
    std::uint64_t count = 0;
    std::memcpy(&count, corrupt.data(), sizeof(count));
    REQUIRE(count > 1);
    --count;
    std::memcpy(corrupt.data(), &count, sizeof(count));
    CHECK_THROWS_AS(backend.RestoreState(corrupt), InferenceError);
    CheckCacheWasCleared(backend);
}

TEST_CASE("A state blob whose token count disagrees with the restored KV data is an InferenceError and leaves a cleared cache", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec;
    spec.maxTokens = 4;
    backend.Complete(SimplePrompt("Name a desert animal."), spec, {}, {});
    std::vector<std::uint8_t> corrupt = backend.SaveState();

    // Drop one token from the list and decrement the count: the llama.cpp payload stays intact, so set_data succeeds
    // but the restored position no longer matches the token list.
    std::uint64_t count = 0;
    std::memcpy(&count, corrupt.data(), sizeof(count));
    REQUIRE(count > 1);
    --count;
    std::memcpy(corrupt.data(), &count, sizeof(count));
    const auto firstToken = corrupt.begin() + static_cast<std::ptrdiff_t>(sizeof(count));
    corrupt.erase(firstToken, firstToken + static_cast<std::ptrdiff_t>(sizeof(std::int32_t)));
    CHECK_THROWS_AS(backend.RestoreState(corrupt), InferenceError);
    CheckCacheWasCleared(backend);
}
