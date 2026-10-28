#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

TEST_CASE("LlamaCppBackend keeps output in the grammar when the model's own choice is rejected", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const Prompt prompt = SimplePrompt("Repeat this word exactly and nothing else: hello");

    // Unconstrained, the model says hello, so the grammar below must reject its preferred first token.
    const Completion unconstrained = backend.Complete(prompt, Greedy(4), {}, {});
    INFO("unconstrained: " << unconstrained.text);
    REQUIRE(unconstrained.text.starts_with("hello"));

    GenerationSpec spec = Greedy(4);
    spec.grammar = R"(root ::= "yes" | "no")";
    const Completion greedy = backend.Complete(prompt, spec, {}, {});
    CHECK((greedy.text == "yes" || greedy.text == "no"));
    CHECK(backend.Complete(prompt, spec, {}, {}).text == greedy.text);

    // Seeded sampling at a high temperature: still always valid, and the same seed gives the same text.
    spec.sampling.temperature = 1.5f;
    for (const std::uint32_t seed : {1u, 2u, 3u, 4u}) {
        spec.sampling.seed = seed;
        const Completion first = backend.Complete(prompt, spec, {}, {});
        CHECK((first.text == "yes" || first.text == "no"));
        CHECK(backend.Complete(prompt, spec, {}, {}).text == first.text);
    }
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
