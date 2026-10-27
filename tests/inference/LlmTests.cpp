#include <catch2/catch_test_macros.hpp>

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
