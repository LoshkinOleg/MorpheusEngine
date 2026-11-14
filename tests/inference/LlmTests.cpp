#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "core/Errors.h"
#include "fakes/FakeBackend.h"
#include "inference/Llm.h"

using namespace morpheus::inference;

TEST_CASE("RoleName spells roles as chat templates expect", "[inference]")
{
    CHECK(RoleName(Role::System) == "system");
    CHECK(RoleName(Role::User) == "user");
    CHECK(RoleName(Role::Assistant) == "assistant");
}

TEST_CASE("FakeBackend counts tokens deterministically through the interface", "[inference]")
{
    morpheus::test::FakeBackend fake(1000);
    LlmBackend& backend = fake;

    CHECK(backend.CountTokens("") == 0);
    CHECK(backend.CountTokens("abcd") == 1);
    CHECK(backend.CountTokens("abcde") == 2);

    Prompt prompt;
    prompt.messages = {{Role::System, "abcd"}, {Role::User, "abcdefgh"}};
    CHECK(backend.CountPromptTokens(prompt) == (1 + 4) + (2 + 4));
    CHECK(backend.ContextSize() == 1000);
    CHECK_THROWS_AS(backend.Complete(prompt, GenerationSpec{}, {}, {}), morpheus::core::ContractError);
}

TEST_CASE("LlmBackend is the union of Tokenizer, ChatModel and Embedder", "[inference]")
{
    static_assert(std::is_base_of_v<Tokenizer, LlmBackend>);
    static_assert(std::is_base_of_v<ChatModel, LlmBackend>);
    static_assert(std::is_base_of_v<Embedder, LlmBackend>);

    // A caller can hold one part alone, as sub-project D's code will.
    morpheus::test::FakeBackend fake(1000);
    Tokenizer& tokenizer = fake;
    CHECK(tokenizer.CountTokens("abcde") == 2);
    CHECK(tokenizer.ContextSize() == 1000);
    ChatModel& chat = fake;
    CHECK_THROWS_AS(chat.SaveState(), morpheus::core::ContractError);
    Embedder& embedder = fake;
    CHECK_THROWS_AS(embedder.EmbeddingModelId(), morpheus::core::ContractError);
}

TEST_CASE("GenerationSpec's slot defaults to the backend's default slot", "[inference]")
{
    CHECK(GenerationSpec{}.slot == -1);
}
